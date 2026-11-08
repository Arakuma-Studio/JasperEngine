// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The Scene view's spline overlay and knot gizmo (docs/specs/S18-splines.md R25, R26): the
// edited spline's curve, screen-scaled knot markers, Bezier handles and its radius band draped
// on the terrain, the Draw Spline tool's stroke, the hit tests the Scene window picks knots,
// handles and the curve with, and the gizmo drag of the selected knots. Screen-scaled markers
// follow OpenEngine's SplineSceneGizmo.cpp (MIT, NOTICE.md), drawn with our line overlays.

#include "render/SceneRender.h"
#include "component/Transform.h"
#include "subsystem/MeshSystem.h"
#include "component/CameraComponent.h"
#include "component/TerrainComponent.h"

#include "Backend.h"
#include "App.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "spline/SplineEditor.h"
#include "spline/SplineEvaluator.h"
#include "subsystem/SplineSystem.h"
#include "util/ScopedDefaultEntityPool.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace{

    const Vector4 kCurveColor(0.20f, 0.85f, 1.00f, 1.0f);
    const Vector4 kCurveIdleColor(0.20f, 0.85f, 1.00f, 0.55f);
    const Vector4 kKnotColor(1.00f, 1.00f, 1.00f, 1.0f);
    const Vector4 kSelectedKnotColor(1.00f, 0.80f, 0.10f, 1.0f);
    const Vector4 kHandleColor(1.00f, 0.45f, 0.75f, 1.0f);
    const Vector4 kBandColor(0.20f, 0.85f, 1.00f, 0.35f);
    const Vector4 kStrokeColor(1.00f, 0.55f, 0.15f, 1.0f);
    // logical pixels
    constexpr float kKnotSize = 9.0f;
    constexpr float kHandleSize = 6.0f;
    constexpr float kCurvePickPixels = 8.0f;

    bool invertible(const Matrix4& m){
        const float det = m.linear().determinant();
        return std::isfinite(det) && std::fabs(det) > 1e-12f;
    }

    Vector3 direction(const Matrix4& m, const Vector3& v){
        return m * v - m * Vector3(0.0f, 0.0f, 0.0f);
    }

}

Entity SceneRender::getKnotEditSpline(const std::vector<Entity>& toolEntities){
    if (editLayer.active || toolEntities.size() != 1 || isPreviewCameraActive()){
        return NULL_ENTITY;
    }
    const SplineEditor& editor = SplineEditor::get();
    const Entity entity = toolEntities[0];
    if (!editor.isEditing(getLatticeSceneId(), entity) || !scene->isEntityCreated(entity)){
        return NULL_ENTITY;
    }
    Transform* transform = scene->findComponent<Transform>(entity);
    if (!transform || !scene->findComponent<SplineComponent>(entity) || !invertible(transform->modelMatrix)){
        return NULL_ENTITY;
    }
    return entity;
}

bool SceneRender::getSplineKnotsPivot(Entity entity, const std::vector<uint32_t>& knots, Vector3& pivot, Quaternion& rotation, OBB& obb){
    const SplineComponent* spline = scene->findComponent<SplineComponent>(entity);
    const Transform* transform = scene->findComponent<Transform>(entity);
    if (!spline || !transform || knots.empty()){
        return false;
    }
    AABB bounds;
    Vector3 sum(0.0f, 0.0f, 0.0f);
    int used = 0;
    for (uint32_t k : knots){
        if (k >= spline->spline.knotCount()){
            continue;
        }
        const Vector3 p = transform->modelMatrix * spline->spline.knot(k).position;
        const float half = getPointHandleHalfSize(p) * 0.5f;
        bounds.merge(AABB(p - Vector3(half), p + Vector3(half)));
        sum += p;
        used++;
    }
    if (used == 0){
        return false;
    }
    // the gizmo stands at the selection's centroid (rotate and scale act about it)
    pivot = sum / static_cast<float>(used);
    rotation = useGlobalTransform ? Quaternion() : transform->worldRotation;
    obb = bounds.getOBB();
    return true;
}

int SceneRender::hitTestSplineKnot(Entity entity, float x, float y){
    const SplineComponent* spline = scene->findComponent<SplineComponent>(entity);
    const Transform* transform = scene->findComponent<Transform>(entity);
    if (!spline || !transform){
        return -1;
    }
    const Ray ray = screenToRay(x, y);
    int best = -1;
    float bestDistance = FLT_MAX;
    for (size_t i = 0; i < spline->spline.knotCount(); i++){
        const Vector3 p = transform->modelMatrix * spline->spline.knot(i).position;
        const float half = billboardScreenScale(p, std::max(kKnotSize, 10.0f) * 0.5f);
        const RayReturn hit = ray.intersects(AABB(p - Vector3(half), p + Vector3(half)));
        if (hit && hit.distance < bestDistance){
            bestDistance = hit.distance;
            best = static_cast<int>(i);
        }
    }
    return best;
}

bool SceneRender::hitTestSplineHandle(Entity entity, float x, float y, uint32_t& knot, bool& out){
    const SplineComponent* spline = scene->findComponent<SplineComponent>(entity);
    const Transform* transform = scene->findComponent<Transform>(entity);
    if (!spline || !transform || spline->spline.type() != spline::SplineType::CubicBezier){
        return false;
    }
    const Ray ray = screenToRay(x, y);
    float bestDistance = FLT_MAX;
    bool found = false;
    // the handles of the selected knots only: those are drawn
    for (uint32_t k : SplineEditor::get().getSelection()){
        if (k >= spline->spline.knotCount()){
            continue;
        }
        const spline::SplineKnot& kn = spline->spline.knot(k);
        for (int side = 0; side < 2; side++){
            const Vector3 tip = transform->modelMatrix * (kn.position + (side == 1 ? kn.tangentOut : kn.tangentIn));
            const float half = billboardScreenScale(tip, std::max(kHandleSize, 10.0f) * 0.5f);
            const RayReturn hit = ray.intersects(AABB(tip - Vector3(half), tip + Vector3(half)));
            if (hit && hit.distance < bestDistance){
                bestDistance = hit.distance;
                knot = k;
                out = side == 1;
                found = true;
            }
        }
    }
    return found;
}

std::vector<uint32_t> SceneRender::getSplineKnotsInRect(Entity entity, Vector2 start, Vector2 end){
    std::vector<uint32_t> inside;
    const SplineComponent* spline = scene->findComponent<SplineComponent>(entity);
    const Transform* transform = scene->findComponent<Transform>(entity);
    if (!spline || !transform){
        return inside;
    }
    const Vector2 lo(std::min(start.x, end.x), std::min(start.y, end.y));
    const Vector2 hi(std::max(start.x, end.x), std::max(start.y, end.y));
    const Matrix4 vp = camera->getViewProjectionMatrix();
    for (size_t i = 0; i < spline->spline.knotCount(); i++){
        const Vector4 clip = vp * Vector4(transform->modelMatrix * spline->spline.knot(i).position, 1.0f);
        if (!(clip.w > 0.0f)){
            continue;
        }
        const Vector3 ndc = Vector3(clip) / clip.w;
        if (ndc.x >= lo.x && ndc.x <= hi.x && ndc.y >= lo.y && ndc.y <= hi.y){
            inside.push_back(static_cast<uint32_t>(i));
        }
    }
    return inside;
}

float SceneRender::worldPerPixelAt(const Vector3& worldPoint) const{
    return billboardScreenScale(worldPoint, 1.0f);
}

bool SceneRender::hitTestSplineCurve(Entity entity, float x, float y, Vector3& worldPoint){
    const SplineComponent* spline = scene->findComponent<SplineComponent>(entity);
    const Transform* transform = scene->findComponent<Transform>(entity);
    if (!spline || !transform || !spline->spline.isValid()){
        return false;
    }
    // the curve's samples projected on the screen: the nearest segment to the cursor
    const float length = spline::length(spline->spline);
    const uint32_t samples = std::clamp<uint32_t>(static_cast<uint32_t>(length / 0.25f) + 2u, 16u, 4096u);
    std::vector<spline::SplineFrame> frames;
    spline::sampleUniform(spline->spline, samples, frames);
    const Matrix4 vp = camera->getViewProjectionMatrix();
    const float width = viewWidth > 0 ? static_cast<float>(viewWidth) : static_cast<float>(Engine::getCanvasWidth());
    const float height = viewHeight > 0 ? static_cast<float>(viewHeight) : static_cast<float>(Engine::getCanvasHeight());
    // the cursor in the same NDC pixels the samples are projected to
    const Ray ray = screenToRay(x, y);
    const Vector4 cursorClip = vp * Vector4(ray.getOrigin() + ray.getDirection() * 1.0f, 1.0f);
    if (!(cursorClip.w > 0.0f)){
        return false;
    }
    const Vector2 cursor(cursorClip.x / cursorClip.w * width * 0.5f, cursorClip.y / cursorClip.w * height * 0.5f);
    float best = kCurvePickPixels * overlayScale;
    bool found = false;
    Vector2 previous;
    bool havePrevious = false;
    Vector3 previousWorld;
    for (const spline::SplineFrame& frame : frames){
        const Vector3 world = transform->modelMatrix * frame.position;
        const Vector4 clip = vp * Vector4(world, 1.0f);
        if (!(clip.w > 0.0f)){
            havePrevious = false;
            continue;
        }
        const Vector2 screen(clip.x / clip.w * width * 0.5f, clip.y / clip.w * height * 0.5f);
        if (havePrevious){
            const Vector2 ab = screen - previous;
            const float len2 = ab.x * ab.x + ab.y * ab.y;
            float t = len2 > 0.0f ? ((cursor.x - previous.x) * ab.x + (cursor.y - previous.y) * ab.y) / len2 : 0.0f;
            t = std::clamp(t, 0.0f, 1.0f);
            const Vector2 q(previous.x + ab.x * t, previous.y + ab.y * t);
            const float d = std::sqrt((q.x - cursor.x) * (q.x - cursor.x) + (q.y - cursor.y) * (q.y - cursor.y));
            if (d < best){
                best = d;
                worldPoint = previousWorld + (world - previousWorld) * t;
                found = true;
            }
        }
        previous = screen;
        previousWorld = world;
        havePrevious = true;
    }
    return found;
}

bool SceneRender::splinePointUnderCursor(Entity entity, float x, float y, bool snapToSurface, Vector3& worldPoint){
    const SplineComponent* spline = scene->findComponent<SplineComponent>(entity);
    const Transform* transform = scene->findComponent<Transform>(entity);
    if (!spline || !transform){
        return false;
    }
    const Ray ray = screenToRay(x, y);
    const bool flat = spline->spline.upAxis().z != 0.0f;
    if (snapToSurface && !flat){
        // the terrain the ray hits first
        auto terrains = scene->getComponentArray<TerrainComponent>();
        float bestDistance = FLT_MAX;
        bool hitSurface = false;
        for (size_t i = 0; i < terrains->size(); i++){
            const Entity terrainEntity = terrains->getEntity(i);
            Transform* terrainTransform = scene->findComponent<Transform>(terrainEntity);
            Vector3 point;
            if (terrainTransform && scene->getSystem<MeshSystem>()->raycastTerrainSurface(ray, terrains->getComponentFromIndex(i), *terrainTransform, point)){
                const float d = (point - ray.getOrigin()).length();
                if (d < bestDistance){
                    bestDistance = d;
                    worldPoint = point;
                    hitSurface = true;
                }
            }
        }
        if (hitSurface){
            return true;
        }
    }
    // the plane through the spline's origin: its local XZ (3D) or XY (2D)
    const Vector3 origin = transform->modelMatrix * Vector3(0.0f, 0.0f, 0.0f);
    const Vector3 normal = direction(transform->modelMatrix, flat ? Vector3(0.0f, 0.0f, 1.0f) : Vector3(0.0f, 1.0f, 0.0f)).normalized();
    const RayReturn hit = ray.intersects(Plane(normal, origin));
    if (!hit){
        return false;
    }
    worldPoint = hit.point;
    return true;
}

void SceneRender::dragSplineHandle(Project* project, uint32_t sceneId, Entity entity, uint32_t knot, bool out, float x, float y){
    const SplineComponent* spline = scene->findComponent<SplineComponent>(entity);
    const Transform* transform = scene->findComponent<Transform>(entity);
    if (!spline || !transform || knot >= spline->spline.knotCount()){
        return;
    }
    const spline::SplineKnot& k = spline->spline.knot(knot);
    const Vector3 tip = transform->modelMatrix * (k.position + (out ? k.tangentOut : k.tangentIn));
    const CameraComponent& cameracomp = scene->getComponent<CameraComponent>(camera->getEntity());
    const Matrix4& vm = cameracomp.viewMatrix;
    Vector3 facing(vm.get(0, 2), vm.get(1, 2), vm.get(2, 2));
    facing = facing.length() > 0.0f ? facing.normalized() : Vector3(0.0f, 0.0f, 1.0f);
    const RayReturn hit = screenToRay(x, y).intersects(Plane(facing, tip));
    if (!hit){
        return;
    }
    if (dragEditStarted && !dragEdit.isOpen()){
        return;
    }
    if (!dragEditStarted){
        dragEdit = CommandHandle::get(sceneId)->beginInteractive(std::string());
        dragEditStarted = true;
    }
    if (std::shared_ptr<SplineSystem> system = scene->findSystem<SplineSystem>()){
        system->setInteractiveEdit(true);
    }
    SplineEditor::get().setTangent(project, knot, out, hit.point, true);
}

void SceneRender::beginSplineDrag(Entity entity, const Vector3& pivot, const Quaternion& rotation){
    splineDrag = SplineDrag();
    const SplineComponent* spline = scene->findComponent<SplineComponent>(entity);
    const Transform* transform = scene->findComponent<Transform>(entity);
    const std::vector<uint32_t>& knots = SplineEditor::get().getSelection();
    if (!spline || !transform || knots.empty() || !invertible(transform->modelMatrix)){
        return;
    }
    splineDrag.active = true;
    splineDrag.spline = entity;
    splineDrag.knots = knots;
    splineDrag.baseKnots = spline->spline.knots();
    splineDrag.model = transform->modelMatrix;
    splineDrag.pivot = pivot;
    splineDrag.rotation = rotation;
    // a drag is its own undo step, whatever came before it
    SplineEditor::endContinuousEdit(getLatticeSceneId());
    if (std::shared_ptr<SplineSystem> system = scene->findSystem<SplineSystem>()){
        system->setInteractiveEdit(true);
    }
}

void SceneRender::dragSplineGizmo(Project* project, uint32_t sceneId, bool invertRotationSnap){
    const SplineComponent* spline = scene->findComponent<SplineComponent>(splineDrag.spline);
    if (!splineDrag.active || !spline || spline->spline.knotCount() != splineDrag.baseKnots.size()){
        // the spline changed under the drag (undo): it ends here
        endSplineDrag();
        return;
    }
    Matrix4 delta;
    if (!gizmoDragDelta(splineDrag.pivot, splineDrag.rotation, invertRotationSnap, delta)){
        return;
    }
    if (dragEditStarted && !dragEdit.isOpen()){
        return;
    }
    if (!dragEditStarted){
        dragEdit = CommandHandle::get(sceneId)->beginInteractive(std::string());
        dragEditStarted = true;
    }
    // from the knots as the drag found them: the gizmo's change is from its start
    const Matrix4& world = splineDrag.model;
    const Matrix4 local = world.inverse();
    SplineEditState state = SplineEditCmd::capture(*spline);
    state.knots = splineDrag.baseKnots;
    const bool scaleRadius = gizmoTools().getGizmoSelected() == GizmoSelected::SCALE && splineDrag.knots.size() == 1;
    const float scale = std::max(direction(delta, Vector3(1.0f, 0.0f, 0.0f)).length(),
                                 std::max(direction(delta, Vector3(0.0f, 1.0f, 0.0f)).length(), direction(delta, Vector3(0.0f, 0.0f, 1.0f)).length()));
    for (uint32_t k : splineDrag.knots){
        if (k >= state.knots.size()){
            continue;
        }
        spline::SplineKnot& knot = state.knots[k];
        if (scaleRadius){
            // Scale on one knot scales its radius (S18 R25)
            knot.radius = std::max(0.0f, splineDrag.baseKnots[k].radius * scale);
            continue;
        }
        knot.position = local * (delta * (world * knot.position));
        knot.tangentIn = direction(local, direction(delta, direction(world, knot.tangentIn)));
        knot.tangentOut = direction(local, direction(delta, direction(world, knot.tangentOut)));
    }
    SplineEditor::setState(project, sceneId, splineDrag.spline, std::move(state), SplineEditKind::MoveKnots, true, splineDrag.knots);
}

void SceneRender::endSplineDrag(){
    if (splineDrag.active){
        const uint32_t sceneId = getLatticeSceneId();
        if (sceneId != NULL_PROJECT_SCENE){
            SplineEditor::endContinuousEdit(sceneId);
        }
    }
    if (std::shared_ptr<SplineSystem> system = scene->findSystem<SplineSystem>()){
        system->setInteractiveEdit(false);
    }
    splineDrag = SplineDrag();
}

void SceneRender::updateSplineOverlay(const std::vector<Entity>& selEntities, Entity editedSpline){
    const SplineEditor& editor = SplineEditor::get();
    const uint32_t sceneId = getLatticeSceneId();
    const bool drawing = editor.isDrawing() && editor.getDrawSceneId() == sceneId;
    std::vector<Entity> splines;
    for (Entity entity : selEntities){
        if (scene->isEntityCreated(entity) && scene->findComponent<SplineComponent>(entity) && scene->findComponent<Transform>(entity)){
            splines.push_back(entity);
        }
    }
    if (drawing && std::find(splines.begin(), splines.end(), editor.getDrawEntity()) == splines.end() &&
        scene->findComponent<SplineComponent>(editor.getDrawEntity())){
        splines.push_back(editor.getDrawEntity());
    }
    if (splineDrag.active && splineDrag.spline != editedSpline){
        splineDrag = SplineDrag();
    }
    if (splines.empty() && !drawing){
        hideSplineOverlay();
        return;
    }
    if (!splineOverlayLines){
        ScopedDefaultEntityPool toolsPool(*toolslayer.getScene(), EntityPool::System);
        splineOverlayLines = new LineRenderer(toolslayer.getScene());
    }
    if (!splineDepthLines){
        ScopedDefaultEntityPool scenePool(*scene, EntityPool::System);
        splineDepthLines = overlay(new LineRenderer(scene));
    }
    splineOverlayLines->clearLines();
    splineDepthLines->clearLines();

    CameraComponent& cameracomp = scene->getComponent<CameraComponent>(camera->getEntity());
    const bool depthTested = cameracomp.type == CameraType::CAMERA_PERSPECTIVE;
    const Matrix4& vm = cameracomp.viewMatrix;
    Vector3 camRight(vm.get(0, 0), vm.get(1, 0), vm.get(2, 0));
    Vector3 camUp(vm.get(0, 1), vm.get(1, 1), vm.get(2, 1));
    camRight = camRight.length() > 0.0f ? camRight.normalized() : Vector3(1, 0, 0);
    camUp = camUp.length() > 0.0f ? camUp.normalized() : Vector3(0, 1, 0);

    auto line = [&](const Vector3& a, const Vector3& b, const Vector4& color){
        splineOverlayLines->addLine(a, b, color);
    };
    auto square = [&](const Vector3& p, float pixels, const Vector4& color, bool filled){
        const float half = billboardScreenScale(p, pixels * 0.5f);
        const Vector3 r = camRight * half;
        const Vector3 u = camUp * half;
        if (filled){
            const int rows = std::max(4, static_cast<int>(std::lround(2.0f * pixels * overlayScale)));
            for (int row = 0; row < rows; row++){
                const float t = (static_cast<float>(row) + 0.5f) / static_cast<float>(rows) * 2.0f - 1.0f;
                line(p - r + u * t, p + r + u * t, color);
            }
        }
        const Vector3 c[4] = {p - r - u, p + r - u, p + r + u, p - r + u};
        for (int i = 0; i < 4; i++){
            line(c[i], c[(i + 1) % 4], color);
        }
    };

    for (Entity entity : splines){
        const SplineComponent& spline = scene->getComponent<SplineComponent>(entity);
        const Transform& transform = scene->getComponent<Transform>(entity);
        const bool edited = entity == editedSpline;
        // the curve
        if (spline.spline.isValid()){
            const float length = spline::length(spline.spline);
            const uint32_t samples = std::clamp<uint32_t>(static_cast<uint32_t>(length / 0.5f) + 2u, 16u, 2048u);
            std::vector<spline::SplineFrame> frames;
            spline::sampleUniform(spline.spline, samples, frames);
            for (size_t i = 1; i < frames.size(); i++){
                line(transform.modelMatrix * frames[i - 1].position, transform.modelMatrix * frames[i].position, edited ? kCurveColor : kCurveIdleColor);
            }
            // the radius band, draped on the terrain (depth tested in 3D)
            if (edited){
                const spline::SplineSurfaceProbe probe = spline.spline.upAxis().z == 0.0f
                    ? SplineEditor::makeProbe(Backend::getApp().getProject()->getScene(sceneId), SplineSnap::Terrain, entity)
                    : spline::SplineSurfaceProbe();
                Vector3 previous[2];
                for (size_t i = 0; i < frames.size(); i++){
                    for (int side = 0; side < 2; side++){
                        const float offset = side == 0 ? -frames[i].radius : frames[i].radius;
                        Vector3 p = transform.modelMatrix * (frames[i].position + frames[i].right * offset);
                        if (probe){
                            const spline::SplineSurfaceSample hit = probe(p);
                            if (hit.hit){
                                p.y = hit.height + 0.05f;
                            }
                        }
                        if (i > 0){
                            if (depthTested){
                                splineDepthLines->addLine(previous[side], p, kBandColor);
                            }else{
                                line(previous[side], p, kBandColor);
                            }
                        }
                        previous[side] = p;
                    }
                }
            }
        }
        // knots: screen-scaled squares; the selected ones filled, with their Bezier handles
        for (size_t i = 0; i < spline.spline.knotCount(); i++){
            const spline::SplineKnot& knot = spline.spline.knot(i);
            const Vector3 p = transform.modelMatrix * knot.position;
            const bool isSelected = edited && editor.isSelected(static_cast<uint32_t>(i));
            square(p, kKnotSize * (isSelected ? 1.15f : 1.0f), isSelected ? kSelectedKnotColor : kKnotColor, isSelected);
            if (isSelected && spline.spline.type() == spline::SplineType::CubicBezier){
                for (const Vector3& handle : {knot.tangentIn, knot.tangentOut}){
                    const Vector3 tip = transform.modelMatrix * (knot.position + handle);
                    line(p, tip, kHandleColor);
                    square(tip, kHandleSize, kHandleColor, true);
                }
            }
        }
    }

    // the Draw Spline tool's stroke or clicked knots
    if (drawing){
        const std::vector<Vector3>& stroke = editor.getStroke();
        for (size_t i = 1; i < stroke.size(); i++){
            line(stroke[i - 1], stroke[i], kStrokeColor);
        }
        const std::vector<spline::SplineKnot>& clicked = editor.getClickKnots();
        for (size_t i = 0; i < clicked.size(); i++){
            square(clicked[i].position, kKnotSize, kStrokeColor, true);
            if (i > 0){
                line(clicked[i - 1].position, clicked[i].position, kStrokeColor);
            }
            if (clicked[i].tangentOut.squaredLength() > 0.0f){
                line(clicked[i].position + clicked[i].tangentIn, clicked[i].position + clicked[i].tangentOut, kHandleColor);
            }
        }
    }

    splineOverlayLines->setVisible(true);
    splineDepthLines->setVisible(depthTested);
}

void SceneRender::hideSplineOverlay(){
    if (splineOverlayLines){
        splineOverlayLines->setVisible(false);
    }
    if (splineDepthLines){
        splineDepthLines->setVisible(false);
    }
}

void SceneRender::destroySplineOverlay(){
    delete splineOverlayLines;
    delete splineDepthLines;
    splineOverlayLines = nullptr;
    splineDepthLines = nullptr;
}
