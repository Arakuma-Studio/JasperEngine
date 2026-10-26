// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FluidEditorTools.h"

#include "Project.h"
#include "Catalog.h"
#include "command/CommandHandle.h"
#include "command/type/PropertyCmd.h"
#include "component/FluidSimulationComponent.h"
#include "component/MeshComponent.h"
#include "component/SpriteComponent.h"
#include "component/Transform.h"
#include "fluid/FluidSimulator.h"
#include "subsystem/FluidSystem.h"

#include <algorithm>
#include <cmath>
#include <memory>

using namespace jasperengine;

namespace {

    struct PreviewEntry {
        uint32_t sceneId;
        Entity entity;
        bool playing;
    };

    std::vector<PreviewEntry> previews;

    struct Brush {
        bool armed = false;
        editor::Project* project = nullptr;
        uint32_t sceneId = NULL_PROJECT_SCENE;
        Entity entity = NULL_ENTITY;
        const Scene* scene = nullptr;
        bool stroking = false;
        bool radiusRun = false;     // the last command is a merged run of [ / ] presses
    };

    Brush brush;

    editor::SceneProject* sceneOf(editor::Project* project, uint32_t sceneId) {
        return project ? project->getScene(sceneId) : nullptr;
    }

    FluidSimulationComponent* fluidOf(editor::SceneProject* sceneProject, Entity entity) {
        if (!sceneProject || !sceneProject->scene || entity == NULL_ENTITY || !sceneProject->scene->isEntityCreated(entity))
            return nullptr;
        return sceneProject->scene->findComponent<FluidSimulationComponent>(entity);
    }

    void setSystemPreview(Scene* scene, Entity entity, bool preview) {
        if (!scene) return;
        if (std::shared_ptr<FluidSystem> fluids = scene->findSystem<FluidSystem>())
            fluids->setPreview(entity, preview);
    }

    std::vector<PreviewEntry>::iterator findPreview(uint32_t sceneId, Entity entity) {
        return std::find_if(previews.begin(), previews.end(), [&](const PreviewEntry& entry) {
            return entry.sceneId == sceneId && entry.entity == entity;
        });
    }

    // the brush's run of radius presses ends: the next command does not merge into it
    void endRadiusRun() {
        if (brush.radiusRun && brush.sceneId != NULL_PROJECT_SCENE)
            editor::CommandHandle::get(brush.sceneId)->mark();
        brush.radiusRun = false;
    }

    // The Sprite quad of an entity: its local rectangle (z = 0) and model matrix
    struct Quad {
        Vector3 min;
        Vector3 max;
        Matrix4 model;
        bool flipY = false;
    };

    bool spriteQuad(Scene* scene, Entity entity, Quad& quad) {
        if (!scene || !scene->isEntityCreated(entity)) return false;
        SpriteComponent* sprite = scene->findComponent<SpriteComponent>(entity);
        MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        Transform* transform = scene->findComponent<Transform>(entity);
        if (!sprite || !mesh || !transform || mesh->aabb.isNull()) return false;
        quad.min = mesh->aabb.getMinimum();
        quad.max = mesh->aabb.getMaximum();
        if (quad.max.x - quad.min.x <= 0.0f || quad.max.y - quad.min.y <= 0.0f) return false;
        quad.model = transform->modelMatrix;
        quad.flipY = sprite->flipY;
        return true;
    }

    // the ray on the quad's plane, in its local space
    bool localHit(const Quad& quad, const Ray& ray, Vector3& hit) {
        const Matrix4 inverse = quad.model.inverse();
        const Vector3 origin = inverse * ray.getOrigin();
        const Vector3 direction = (inverse * (ray.getOrigin() + ray.getDirection())) - origin;
        if (std::fabs(direction.z) < 1e-8f) return false;
        const float t = -origin.z / direction.z;
        if (!std::isfinite(t) || t < 0.0f) return false;
        hit = origin + direction * t;
        return true;
    }

}

// ---- preview

editor::FluidPreviewState editor::FluidEditorTools::getPreviewState(uint32_t sceneId, Entity entity) {
    auto it = findPreview(sceneId, entity);
    if (it == previews.end()) return FluidPreviewState::STOPPED;
    return it->playing ? FluidPreviewState::PLAYING : FluidPreviewState::PAUSED;
}

void editor::FluidEditorTools::playPreview(Project* project, uint32_t sceneId, Entity entity) {
    SceneProject* sceneProject = sceneOf(project, sceneId);
    if (!fluidOf(sceneProject, entity) || sceneProject->playState != ScenePlayState::STOPPED) return;
    auto it = findPreview(sceneId, entity);
    if (it == previews.end()) previews.push_back({sceneId, entity, true});
    else it->playing = true;
    setSystemPreview(sceneProject->scene, entity, true);
    sceneProject->needUpdateRender = true;
}

void editor::FluidEditorTools::pausePreview(Project* project, uint32_t sceneId, Entity entity) {
    SceneProject* sceneProject = sceneOf(project, sceneId);
    auto it = findPreview(sceneId, entity);
    if (it == previews.end()) return;
    it->playing = false;
    if (fluidOf(sceneProject, entity)) {
        setSystemPreview(sceneProject->scene, entity, false);
        sceneProject->needUpdateRender = true;
    }
}

void editor::FluidEditorTools::stopPreview(Project* project, uint32_t sceneId, Entity entity) {
    auto it = findPreview(sceneId, entity);
    if (it != previews.end()) previews.erase(it);

    if (brush.stroking && brush.sceneId == sceneId && brush.entity == entity) endStroke();

    SceneProject* sceneProject = sceneOf(project, sceneId);
    if (FluidSimulationComponent* fluid = fluidOf(sceneProject, entity)) {
        setSystemPreview(sceneProject->scene, entity, false);
        // back to how the scene starts
        fluid->needReset = true;
        fluid->pendingSplats.clear();
        fluid->pendingBursts.clear();
        fluid->pendingSteps = 0;
        if (fluid->simulator) fluid->simulator->pointerCancelAll();
        sceneProject->needUpdateRender = true;
    }
}

void editor::FluidEditorTools::stopAll(Project* project) {
    const std::vector<PreviewEntry> entries = previews;
    for (const PreviewEntry& entry : entries) {
        SceneProject* sceneProject = sceneOf(project, entry.sceneId);
        // a running scene is the game's now: only the flag goes (Stop restores the scene)
        if (sceneProject && sceneProject->playState != ScenePlayState::STOPPED) {
            setSystemPreview(sceneProject->scene, entry.entity, false);
            continue;
        }
        stopPreview(project, entry.sceneId, entry.entity);
    }
    previews.clear();
    disarm();
}

void editor::FluidEditorTools::forgetScene(uint32_t sceneId) {
    previews.erase(std::remove_if(previews.begin(), previews.end(), [&](const PreviewEntry& entry) {
        return entry.sceneId == sceneId;
    }), previews.end());
    if (brush.armed && brush.sceneId == sceneId) {
        brush = Brush();
    }
}

void editor::FluidEditorTools::update(Project* project) {
    if (!project) return;

    const std::vector<PreviewEntry> entries = previews;
    for (const PreviewEntry& entry : entries) {
        SceneProject* sceneProject = sceneOf(project, entry.sceneId);
        if (!sceneProject || !sceneProject->scene) {
            forgetScene(entry.sceneId);
            continue;
        }
        FluidSimulationComponent* fluid = fluidOf(sceneProject, entry.entity);
        if (!fluid) {
            // the GameObject or its component went (FluidSystem dropped it with them)
            auto it = findPreview(entry.sceneId, entry.entity);
            if (it != previews.end()) previews.erase(it);
            continue;
        }
        if (sceneProject->playState != ScenePlayState::STOPPED) {
            // Play started: the game runs it from its start
            setSystemPreview(sceneProject->scene, entry.entity, false);
            auto it = findPreview(entry.sceneId, entry.entity);
            if (it != previews.end()) previews.erase(it);
            continue;
        }
        // a playing preview moves every frame; a paused one draws what is queued for it
        if (entry.playing || fluid->needReset || !fluid->pendingSplats.empty() || !fluid->pendingBursts.empty() ||
            fluid->pendingSteps > 0) {
            sceneProject->needUpdateRender = true;
        }
    }

    if (brush.armed) {
        SceneProject* sceneProject = sceneOf(project, brush.sceneId);
        const bool selected = sceneProject && project->isSelectedEntity(brush.sceneId, brush.entity) &&
                              project->getSelectedEntities(brush.sceneId).size() == 1;
        if (!selected || !canPaint(project, brush.sceneId, brush.entity)) {
            disarm();
        }
    }
}

// ---- Paint Dye brush

bool editor::FluidEditorTools::canPaint(Project* project, uint32_t sceneId, Entity entity, std::string* reason) {
    auto fail = [&](const char* text) {
        if (reason) *reason = text;
        return false;
    };
    SceneProject* sceneProject = sceneOf(project, sceneId);
    FluidSimulationComponent* fluid = fluidOf(sceneProject, entity);
    if (!fluid) return fail("No Fluid Simulation.");
    if (sceneProject->playState != ScenePlayState::STOPPED)
        return fail("Paint Dye works in edit mode. While the game plays, an Interactive fluid takes the game's own pointer.");
    if (fluid->output != FluidOutput::MATERIAL)
        return fail("Paint Dye paints on the fluid's Sprite: set Output to Material.");
    Quad quad;
    if (!spriteQuad(sceneProject->scene, entity, quad))
        return fail("Paint Dye paints on the fluid's Sprite: this GameObject has no Sprite showing the fluid.");
    return true;
}

bool editor::FluidEditorTools::arm(Project* project, uint32_t sceneId, Entity entity) {
    if (!canPaint(project, sceneId, entity)) return false;
    if (brush.armed && (brush.sceneId != sceneId || brush.entity != entity)) disarm();
    SceneProject* sceneProject = sceneOf(project, sceneId);
    brush.armed = true;
    brush.project = project;
    brush.sceneId = sceneId;
    brush.entity = entity;
    brush.scene = sceneProject->scene;
    brush.stroking = false;
    brush.radiusRun = false;
    sceneProject->needUpdateRender = true;
    return true;
}

void editor::FluidEditorTools::disarm() {
    if (!brush.armed) return;
    endStroke();
    endRadiusRun();
    if (SceneProject* sceneProject = sceneOf(brush.project, brush.sceneId))
        sceneProject->needUpdateRender = true;  // the cursor goes
    brush = Brush();
}

bool editor::FluidEditorTools::isArmed(uint32_t sceneId, Entity entity) {
    return brush.armed && brush.sceneId == sceneId && brush.entity == entity;
}

bool editor::FluidEditorTools::isPainting(const Scene* scene) {
    return brush.armed && scene && brush.scene == scene;
}

bool editor::FluidEditorTools::beginStroke(Scene* scene, const Ray& ray) {
    if (!isPainting(scene)) return false;
    SceneProject* sceneProject = sceneOf(brush.project, brush.sceneId);
    FluidSimulationComponent* fluid = fluidOf(sceneProject, brush.entity);
    if (!fluid) return false;

    Vector2 position;
    if (!rayToFluid(sceneProject->scene, brush.entity, ray, true, position)) return false;

    endRadiusRun();
    if (!fluid->simulator) fluid->simulator = std::make_shared<FluidSimulator>();
    if (getPreviewState(brush.sceneId, brush.entity) != FluidPreviewState::PLAYING)
        playPreview(brush.project, brush.sceneId, brush.entity);
    fluid->simulator->pointerDown(BRUSH_POINTER, position);
    brush.stroking = true;
    sceneProject->needUpdateRender = true;
    return true;
}

void editor::FluidEditorTools::paintStroke(Scene* scene, const Ray& ray) {
    if (!brush.stroking || !isPainting(scene)) return;
    SceneProject* sceneProject = sceneOf(brush.project, brush.sceneId);
    FluidSimulationComponent* fluid = fluidOf(sceneProject, brush.entity);
    if (!fluid || !fluid->simulator) return;

    // past the quad's edge the stroke goes on (its splats fall off the image)
    Vector2 position;
    if (rayToFluid(sceneProject->scene, brush.entity, ray, false, position)) {
        fluid->simulator->pointerMove(BRUSH_POINTER, position);
        sceneProject->needUpdateRender = true;
    }
}

void editor::FluidEditorTools::endStroke() {
    if (!brush.stroking) return;
    brush.stroking = false;
    FluidSimulationComponent* fluid = fluidOf(sceneOf(brush.project, brush.sceneId), brush.entity);
    if (fluid && fluid->simulator) fluid->simulator->pointerUp(BRUSH_POINTER);
}

void editor::FluidEditorTools::adjustRadius(float factor) {
    if (!brush.armed) return;
    FluidSimulationComponent* fluid = fluidOf(sceneOf(brush.project, brush.sceneId), brush.entity);
    if (!fluid) return;

    const float current = fluid->splatRadius;
    const float value = std::clamp(current * factor, 0.01f, std::max(1.0f, current));
    if (value == current) return;

    CommandHistory* history = CommandHandle::get(brush.sceneId);
    if (!brush.radiusRun) history->mark();
    history->addCommand(new PropertyCmd<float>(brush.project, brush.sceneId, brush.entity, ComponentType::FluidSimulationComponent,
                                               "splatRadius", value));
    brush.radiusRun = true;
}

bool editor::FluidEditorTools::cursorCircle(const Scene* scene, const Ray& ray, std::vector<Vector3>& points) {
    points.clear();
    if (!isPainting(scene)) return false;
    SceneProject* sceneProject = sceneOf(brush.project, brush.sceneId);
    FluidSimulationComponent* fluid = fluidOf(sceneProject, brush.entity);
    Quad quad;
    if (!fluid || !spriteQuad(sceneProject->scene, brush.entity, quad)) return false;

    Vector3 hit;
    if (!localHit(quad, ray, hit)) return false;
    const float width = quad.max.x - quad.min.x;
    const float height = quad.max.y - quad.min.y;
    // over the quad only (a stroke started on it goes on past its edges)
    if (!brush.stroking && (hit.x < quad.min.x || hit.x > quad.max.x || hit.y < quad.min.y || hit.y > quad.max.y)) return false;

    // Pavel's splat: exp(-dot(p, p) / r) with p.x scaled by the aspect ratio (distances in
    // heights of the image) and r = splatRadius / 100, times the aspect when it is above 1.
    // The circle is where it falls to 1/e: sqrt(r) heights from the centre.
    const float aspect = fluid->aspectRatio > 0.0f ? fluid->aspectRatio : width / height;
    const float r = fluid->splatRadius / 100.0f * std::max(aspect, 1.0f);
    const float reach = std::sqrt(std::max(r, 0.0f));
    const float radiusX = reach / aspect * width;
    const float radiusY = reach * height;

    const int segments = 48;
    points.reserve(segments);
    for (int i = 0; i < segments; i++) {
        const float angle = (float)i / (float)segments * 2.0f * 3.14159265f;
        const Vector3 local(hit.x + std::cos(angle) * radiusX, hit.y + std::sin(angle) * radiusY, hit.z);
        points.push_back(quad.model * local);
    }
    return true;
}

bool editor::FluidEditorTools::spriteCorners(Scene* scene, Entity entity, std::vector<Vector3>& corners) {
    corners.clear();
    Quad quad;
    if (!spriteQuad(scene, entity, quad)) return false;
    corners.push_back(quad.model * Vector3(quad.min.x, quad.min.y, 0.0f));
    corners.push_back(quad.model * Vector3(quad.max.x, quad.min.y, 0.0f));
    corners.push_back(quad.model * Vector3(quad.max.x, quad.max.y, 0.0f));
    corners.push_back(quad.model * Vector3(quad.min.x, quad.max.y, 0.0f));
    return true;
}

bool editor::FluidEditorTools::rayToFluid(Scene* scene, Entity entity, const Ray& ray, bool requireInside, Vector2& fluidPos, Vector3* worldPos) {
    Quad quad;
    if (!spriteQuad(scene, entity, quad)) return false;
    Vector3 hit;
    if (!localHit(quad, ray, hit)) return false;

    const float s = (hit.x - quad.min.x) / (quad.max.x - quad.min.x);
    const float t = (hit.y - quad.min.y) / (quad.max.y - quad.min.y);
    if (requireInside && (s < 0.0f || s > 1.0f || t < 0.0f || t > 1.0f)) return false;

    // The Sprite's texture coordinates (MeshSystem::createSprite): with flipY (world cameras)
    // its lowest local y shows the image's bottom row, without it (UI cameras, y down) its top
    fluidPos = Vector2(s, quad.flipY ? t : 1.0f - t);
    if (worldPos) *worldPos = quad.model * hit;
    return true;
}
