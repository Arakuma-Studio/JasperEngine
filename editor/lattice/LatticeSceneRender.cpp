// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// SceneRender's lattice part (spec §9.2): the cages of the selected lattices (all of them
// with the Gizmos popup's "Lattices"), the gizmo on the selected points and its drag.

#include "render/SceneRender.h"

#include "AppSettings.h"
#include "Backend.h"
#include "App.h"
#include "Project.h"
#include "PrefabStage.h"
#include "lattice/LatticeEditorOps.h"
#include "lattice/LatticeSolver.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <functional>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace{

    // faint cages of the lattices drawn without being selected
    constexpr float unselectedAlpha = 0.35f;
    // the rest cage drawn under a deformed one
    constexpr float restCageAlpha = 0.22f;
    // above this many points the unselected ones are outlines, not filled squares
    constexpr int filledPointsLimit = 512;

    Vector4 toVector4(const std::array<float, 4>& color){
        return Vector4(color[0], color[1], color[2], color[3]);
    }

    Vector4 mix(const Vector4& a, const Vector4& b, float t){
        return a + (b - a) * t;
    }

    Vector4 withAlpha(Vector4 color, float factor){
        color.w *= factor;
        return color;
    }

    bool isInvertible(const Matrix4& m){
        float det = m.linear().determinant();
        return std::isfinite(det) && std::fabs(det) > 1e-12f;
    }

    // edge colour by length / rest length: squash colour at half, the line colour at rest,
    // stretch colour at twice (log scale, so halving and doubling weigh the same)
    Vector4 stretchColor(const LatticeViewSettings& view, const Vector4& lineColor, float ratio){
        if (!view.stretchColoring || !(ratio > 0.0f) || !std::isfinite(ratio)){
            return lineColor;
        }
        float t = std::clamp(std::log2(ratio), -1.0f, 1.0f);
        Vector4 color = t < 0.0f ? mix(lineColor, toVector4(view.squashColor), -t) : mix(lineColor, toVector4(view.stretchColor), t);
        color.w = lineColor.w;
        return color;
    }

}

uint32_t SceneRender::getLatticeSceneId(){
    if (latticeSceneIdKnown){
        return latticeSceneId;
    }
    Project* project = Backend::getApp().getProject();
    if (!project){
        return NULL_PROJECT_SCENE;
    }
    for (SceneProject& sceneProject : project->getScenes()){
        if (sceneProject.sceneRender == this){
            latticeSceneId = sceneProject.id;
            latticeSceneIdKnown = true;
            return latticeSceneId;
        }
    }
    for (size_t i = 0; i < project->getPrefabStageCount(); i++){
        PrefabStage* stage = project->getPrefabStageAt(i);
        if (stage && stage->sceneProject.sceneRender == this){
            latticeSceneId = stage->sceneProject.id;
            latticeSceneIdKnown = true;
            return latticeSceneId;
        }
    }
    return NULL_PROJECT_SCENE;
}

Entity SceneRender::getPointEditLattice(const std::vector<Entity>& toolEntities){
    if (editLayer.active || toolEntities.size() != 1 || isPreviewCameraActive()){
        return NULL_ENTITY;
    }
    Entity entity = toolEntities[0];
    if (!scene->isEntityCreated(entity)){
        return NULL_ENTITY;
    }
    Transform* transform = scene->findComponent<Transform>(entity);
    LatticeComponent* lattice = scene->findComponent<LatticeComponent>(entity);
    if (!transform || !lattice || !isInvertible(transform->modelMatrix)){
        return NULL_ENTITY;
    }
    if ((int)lattice->offsets.size() != lattice->resolutionX * lattice->resolutionY * lattice->resolutionZ){
        return NULL_ENTITY;
    }
    return entity;
}

bool SceneRender::getLatticeWorldPoints(Entity entity, std::vector<Vector3>& points){
    Transform* transform = scene->findComponent<Transform>(entity);
    LatticeComponent* lattice = scene->findComponent<LatticeComponent>(entity);
    if (!transform || !lattice){
        return false;
    }
    const int count = lattice->resolutionX * lattice->resolutionY * lattice->resolutionZ;
    if (count <= 0 || (int)lattice->offsets.size() != count){
        return false;
    }
    points.resize((size_t)count);
    for (int index = 0; index < count; index++){
        Vector3 local;
        LatticeEditorOps::getPointLocalPosition(*lattice, index, local);
        points[(size_t)index] = transform->modelMatrix * local;
    }
    return true;
}

bool SceneRender::getLatticePointsPivot(Entity entity, const std::vector<int>& points, Vector3& pivot, Quaternion& rotation, OBB& obb){
    std::vector<Vector3> world;
    if (points.empty() || !getLatticeWorldPoints(entity, world)){
        return false;
    }
    AABB bounds;
    Vector3 sum;
    int used = 0;
    for (int index : points){
        if (index < 0 || index >= (int)world.size()){
            continue;
        }
        const Vector3& p = world[(size_t)index];
        float half = getPointHandleHalfSize(p) * 0.5f;
        bounds.merge(AABB(p - Vector3(half), p + Vector3(half)));
        sum += p;
        used++;
    }
    if (used == 0){
        return false;
    }
    // the editor has no pivot/center toggle: the gizmo is at the centroid of the points
    pivot = sum / (float)used;
    rotation = Quaternion();
    if (!useGlobalTransform){
        rotation = scene->getComponent<Transform>(entity).worldRotation;
    }
    obb = bounds.getOBB();
    return true;
}

int SceneRender::hitTestLatticePoint(Entity entity, float x, float y){
    std::vector<Vector3> world;
    if (!getLatticeWorldPoints(entity, world)){
        return -1;
    }
    Transform& transform = scene->getComponent<Transform>(entity);
    if (!isInvertible(transform.modelMatrix)){
        return -1;
    }
    const float pickSize = std::max(AppSettings::getLatticeViewSettings().pointSize, 10.0f);

    Ray ray = screenToRay(x, y);
    int bestIndex = -1;
    float bestDistance = FLT_MAX;
    for (size_t i = 0; i < world.size(); i++){
        float halfSize = billboardScreenScale(world[i], pickSize * 0.5f);
        AABB handleAABB(world[i] - Vector3(halfSize), world[i] + Vector3(halfSize));
        RayReturn rr = ray.intersects(handleAABB);
        if (rr && rr.distance < bestDistance){
            bestDistance = rr.distance;
            bestIndex = (int)i;
        }
    }
    return bestIndex;
}

std::vector<int> SceneRender::getLatticePointsInRect(Entity entity, Vector2 start, Vector2 end){
    std::vector<int> inside;
    std::vector<Vector3> world;
    if (!getLatticeWorldPoints(entity, world)){
        return inside;
    }
    const Vector2 minRect(std::min(start.x, end.x), std::min(start.y, end.y));
    const Vector2 maxRect(std::max(start.x, end.x), std::max(start.y, end.y));
    const Matrix4 vpMatrix = camera->getViewProjectionMatrix();
    for (size_t i = 0; i < world.size(); i++){
        Vector4 clip = vpMatrix * Vector4(world[i], 1.0f);
        if (!(clip.w > 0.0f)){
            continue; // behind the camera
        }
        Vector3 ndc = Vector3(clip) / clip.w;
        if (ndc.x >= minRect.x && ndc.x <= maxRect.x && ndc.y >= minRect.y && ndc.y <= maxRect.y){
            inside.push_back((int)i);
        }
    }
    return inside;
}

AABB SceneRender::getLatticePointsAABB(Entity entity, const std::vector<int>& points){
    AABB bounds;
    std::vector<Vector3> world;
    if (!getLatticeWorldPoints(entity, world)){
        return bounds;
    }
    if (points.empty()){
        for (const Vector3& p : world){
            bounds.merge(p);
        }
    }else{
        for (int index : points){
            if (index >= 0 && index < (int)world.size()){
                bounds.merge(world[(size_t)index]);
            }
        }
    }
    return bounds;
}

void SceneRender::drawLatticeCage(LatticeCageLines& lines, Entity entity, bool selected, bool editable){
    lines.overlay->clearLines();
    lines.depth->clearLines();

    Transform& transform = scene->getComponent<Transform>(entity);
    LatticeComponent& lattice = scene->getComponent<LatticeComponent>(entity);
    const int rx = lattice.resolutionX;
    const int ry = lattice.resolutionY;
    const int rz = lattice.resolutionZ;
    const int count = rx * ry * rz;
    if (rx < LATTICE_MIN_RESOLUTION || ry < LATTICE_MIN_RESOLUTION || rz < LATTICE_MIN_RESOLUTION || (int)lattice.offsets.size() != count){
        lines.overlay->setVisible(false);
        lines.depth->setVisible(false);
        return;
    }

    const LatticeViewSettings view = AppSettings::getLatticeViewSettings();
    CameraComponent& cameracomp = scene->getComponent<CameraComponent>(camera->getEntity());
    // a perspective view draws the hidden parts faded over a depth-tested cage; 2D views
    // (flat sprites inside a thin cage) draw it all over the scene
    const bool depthTested = cameracomp.type == CameraType::CAMERA_PERSPECTIVE;
    const float overlayFactor = depthTested ? view.behindOpacity : 1.0f;

    const Matrix4& vm = cameracomp.viewMatrix;
    Vector3 camRight(vm.get(0, 0), vm.get(1, 0), vm.get(2, 0));
    Vector3 camUp(vm.get(0, 1), vm.get(1, 1), vm.get(2, 1));
    Vector3 camBack(vm.get(0, 2), vm.get(1, 2), vm.get(2, 2));
    camRight = camRight.length() > 0.0f ? camRight.normalized() : Vector3(1, 0, 0);
    camUp = camUp.length() > 0.0f ? camUp.normalized() : Vector3(0, 1, 0);
    camBack = camBack.length() > 0.0f ? camBack.normalized() : Vector3(0, 0, 1);

    const float alpha = selected ? 1.0f : unselectedAlpha;
    const Vector4 lineColor = withAlpha(toVector4(view.lineColor), alpha);
    const int thickness = std::max(1, (int)std::lround(view.lineThickness * overlayScale));

    auto addLine = [&](const Vector3& a, const Vector3& b, const Vector4& color){
        if (thickness == 1){
            lines.depth->addLine(a, b, color);
            lines.overlay->addLine(a, b, withAlpha(color, overlayFactor));
            return;
        }
        // thicker lines: parallel copies one pixel apart across the edge on screen
        Vector3 side = (b - a).crossProduct(camBack);
        side = side.length() > 1e-12f ? side.normalized() : camRight;
        for (int n = 0; n < thickness; n++){
            float shift = (float)n - (float)(thickness - 1) * 0.5f;
            Vector3 oa = side * (billboardScreenScale(a, 1.0f) * shift);
            Vector3 ob = side * (billboardScreenScale(b, 1.0f) * shift);
            lines.depth->addLine(a + oa, b + ob, color);
            lines.overlay->addLine(a + oa, b + ob, withAlpha(color, overlayFactor));
        }
    };

    std::vector<Vector3> rest((size_t)count);
    std::vector<Vector3> world((size_t)count);
    bool deformed = false;
    for (int k = 0; k < rz; k++){
        for (int j = 0; j < ry; j++){
            for (int i = 0; i < rx; i++){
                int index = i + rx * (j + ry * k);
                Vector3 restLocal = lattice::restPosition(rx, ry, rz, i, j, k);
                rest[(size_t)index] = transform.modelMatrix * restLocal;
                world[(size_t)index] = transform.modelMatrix * (restLocal + lattice.offsets[(size_t)index]);
                if (lattice.offsets[(size_t)index] != Vector3(0, 0, 0)){
                    deformed = true;
                }
            }
        }
    }

    auto forEachEdge = [&](const std::function<void(int, int)>& edge){
        for (int k = 0; k < rz; k++){
            for (int j = 0; j < ry; j++){
                for (int i = 0; i < rx; i++){
                    int index = i + rx * (j + ry * k);
                    if (i + 1 < rx) edge(index, index + 1);
                    if (j + 1 < ry) edge(index, index + rx);
                    if (k + 1 < rz) edge(index, index + rx * ry);
                }
            }
        }
    };

    // the rest cage, faint, under a deformed one
    if (deformed && selected){
        const Vector4 restColor = withAlpha(toVector4(view.lineColor), restCageAlpha);
        forEachEdge([&](int a, int b){
            lines.depth->addLine(rest[(size_t)a], rest[(size_t)b], restColor);
            lines.overlay->addLine(rest[(size_t)a], rest[(size_t)b], withAlpha(restColor, overlayFactor));
        });
    }

    forEachEdge([&](int a, int b){
        float restLength = rest[(size_t)a].distance(rest[(size_t)b]);
        float length = world[(size_t)a].distance(world[(size_t)b]);
        float ratio = restLength > 1e-12f ? length / restLength : 1.0f;
        addLine(world[(size_t)a], world[(size_t)b], stretchColor(view, lineColor, ratio));
    });

    // points: screen-constant squares facing the camera
    if (selected){
        const std::vector<int> none;
        const std::vector<int>& selectedPoints = editable ? LatticeEditorOps::getSelectedPoints(getLatticeSceneId(), entity) : none;
        const Vector4 pointColor = toVector4(view.pointColor);
        const Vector4 selectedColor = toVector4(view.selectedPointColor);
        const int rows = std::max(2, (int)std::lround(view.pointSize * overlayScale));
        for (int index = 0; index < count; index++){
            const Vector3& p = world[(size_t)index];
            const bool isSelected = std::binary_search(selectedPoints.begin(), selectedPoints.end(), index);
            const Vector4& color = isSelected ? selectedColor : pointColor;
            const float half = billboardScreenScale(p, view.pointSize * 0.5f) * (isSelected ? 1.15f : 1.0f);
            const Vector3 r = camRight * half;
            const Vector3 u = camUp * half;
            const bool filled = isSelected || count <= filledPointsLimit;
            if (filled){
                for (int row = 0; row < rows; row++){
                    float t = ((float)row + 0.5f) / (float)rows * 2.0f - 1.0f;
                    lines.depth->addLine(p - r + u * t, p + r + u * t, color);
                    lines.overlay->addLine(p - r + u * t, p + r + u * t, withAlpha(color, overlayFactor));
                }
            }
            const Vector3 corners[4] = {p - r - u, p + r - u, p + r + u, p - r + u};
            for (int c = 0; c < 4; c++){
                lines.depth->addLine(corners[c], corners[(c + 1) % 4], color);
                lines.overlay->addLine(corners[c], corners[(c + 1) % 4], withAlpha(color, overlayFactor));
            }
        }
    }

    lines.depth->setVisible(depthTested);
    lines.overlay->setVisible(true);
}

void SceneRender::drawLatticeFalloff(Entity entity, bool visible){
    if (!latticeFalloffLines){
        if (!visible){
            return;
        }
        ScopedDefaultEntityPool sys(*toolslayer.getScene(), EntityPool::System);
        latticeFalloffLines = new LineRenderer(toolslayer.getScene());
    }
    latticeFalloffLines->clearLines();

    const uint32_t sceneId = getLatticeSceneId();
    const LatticeFalloff& falloff = LatticeEditorOps::falloff();
    const std::vector<int>& points = LatticeEditorOps::getSelectedPoints(sceneId, entity);
    Vector3 pivot;
    Quaternion rotation;
    OBB obb;
    if (!visible || !falloff.enabled || !(falloff.radius > 0.0f) || !getLatticePointsPivot(entity, points, pivot, rotation, obb)){
        latticeFalloffLines->setVisible(false);
        return;
    }
    // the radius is in lattice local units: a local circle facing the camera, mapped to the world
    const Matrix4& model = scene->getComponent<Transform>(entity).modelMatrix;
    const Matrix4 inverse = model.inverse();
    CameraComponent& cameracomp = scene->getComponent<CameraComponent>(camera->getEntity());
    const Matrix4& vm = cameracomp.viewMatrix;
    Vector3 viewAxis = inverse.linear() * Vector3(vm.get(0, 2), vm.get(1, 2), vm.get(2, 2));
    if (!(viewAxis.length() > 1e-12f)){
        viewAxis = Vector3(0, 0, 1);
    }
    viewAxis = viewAxis.normalized();
    Vector3 helper = std::fabs(viewAxis.y) < 0.9f ? Vector3(0, 1, 0) : Vector3(1, 0, 0);
    Vector3 a = viewAxis.crossProduct(helper).normalized();
    Vector3 b = viewAxis.crossProduct(a).normalized();

    const Vector3 center = inverse * pivot;
    const Vector4 color = withAlpha(toVector4(AppSettings::getLatticeViewSettings().selectedPointColor), 0.85f);
    const int segments = 48;
    for (int s = 0; s < segments; s++){
        float a0 = 2.0f * (float)M_PI * (float)s / (float)segments;
        float a1 = 2.0f * (float)M_PI * (float)(s + 1) / (float)segments;
        Vector3 p0 = model * (center + (a * std::cos(a0) + b * std::sin(a0)) * falloff.radius);
        Vector3 p1 = model * (center + (a * std::cos(a1) + b * std::sin(a1)) * falloff.radius);
        latticeFalloffLines->addLine(p0, p1, color);
    }
    latticeFalloffLines->setVisible(true);
}

void SceneRender::updateLatticeCages(const std::vector<Entity>& selEntities, const std::vector<Entity>& entities, Entity editedLattice){
    const uint32_t sceneId = getLatticeSceneId();
    if (sceneId != NULL_PROJECT_SCENE){
        int count = 0;
        if (LatticeComponent* lattice = scene->findComponent<LatticeComponent>(editedLattice)){
            count = lattice->resolutionX * lattice->resolutionY * lattice->resolutionZ;
        }
        LatticeEditorOps::validateSelection(sceneId, editedLattice, count);
    }
    if (latticeDrag.active && latticeDrag.lattice != editedLattice){
        latticeDrag = LatticeDrag();
    }

    std::set<Entity> selected(selEntities.begin(), selEntities.end());
    std::set<Entity> drawn;

    auto draw = [&](Entity entity, bool isSelected){
        if (drawn.count(entity) || !scene->isEntityCreated(entity)){
            return;
        }
        if (!scene->findComponent<LatticeComponent>(entity) || !scene->findComponent<Transform>(entity)){
            return;
        }
        drawn.insert(entity);
        LatticeCageLines& lines = latticeCages[entity];
        if (!lines.overlay){
            ScopedDefaultEntityPool toolsPool(*toolslayer.getScene(), EntityPool::System);
            lines.overlay = new LineRenderer(toolslayer.getScene());
        }
        if (!lines.depth){
            ScopedDefaultEntityPool scenePool(*scene, EntityPool::System);
            lines.depth = overlay(new LineRenderer(scene));
        }
        drawLatticeCage(lines, entity, isSelected, entity == editedLattice);
    };

    // a selected lattice shows whatever the other overlays are set to
    for (Entity entity : selEntities){
        draw(entity, true);
    }
    if (displaySettings.showAllLattices){
        auto lattices = scene->getComponentArray<LatticeComponent>();
        std::set<Entity> authored(entities.begin(), entities.end());
        for (size_t i = 0; i < lattices->size(); i++){
            Entity entity = lattices->getEntity(i);
            Transform* transform = scene->findComponent<Transform>(entity);
            if (!authored.count(entity) && !isPlaying){
                continue;
            }
            if (transform && transform->visible && !selected.count(entity)){
                draw(entity, false);
            }
        }
    }

    for (auto it = latticeCages.begin(); it != latticeCages.end();){
        if (!drawn.count(it->first)){
            delete it->second.overlay;
            delete it->second.depth;
            it = latticeCages.erase(it);
        }else{
            ++it;
        }
    }

    drawLatticeFalloff(editedLattice, editedLattice != NULL_ENTITY);
}

void SceneRender::hideLatticeCages(){
    for (auto& [entity, lines] : latticeCages){
        lines.overlay->setVisible(false);
        lines.depth->setVisible(false);
    }
    if (latticeFalloffLines){
        latticeFalloffLines->setVisible(false);
    }
}

void SceneRender::destroyLatticeCages(){
    for (auto& [entity, lines] : latticeCages){
        delete lines.overlay;
        delete lines.depth;
    }
    latticeCages.clear();
    delete latticeFalloffLines;
    latticeFalloffLines = nullptr;
}

void SceneRender::beginLatticeDrag(Entity entity, const Vector3& pivot, const Quaternion& rotation){
    latticeDrag = LatticeDrag();
    const uint32_t sceneId = getLatticeSceneId();
    LatticeComponent* lattice = scene->findComponent<LatticeComponent>(entity);
    Transform* transform = scene->findComponent<Transform>(entity);
    if (sceneId == NULL_PROJECT_SCENE || !lattice || !transform || !isInvertible(transform->modelMatrix)){
        return;
    }
    const std::vector<int>& points = LatticeEditorOps::getSelectedPoints(sceneId, entity);
    if (points.empty()){
        return;
    }
    latticeDrag.active = true;
    latticeDrag.lattice = entity;
    latticeDrag.points = points;
    latticeDrag.baseOffsets = lattice->offsets;
    latticeDrag.resolution[0] = lattice->resolutionX;
    latticeDrag.resolution[1] = lattice->resolutionY;
    latticeDrag.resolution[2] = lattice->resolutionZ;
    latticeDrag.latticeModel = transform->modelMatrix;
    latticeDrag.pivot = pivot;
    latticeDrag.rotation = rotation;
    // a drag is its own undo step, whatever came before it
    LatticeEditorOps::endContinuousEdit(sceneId);
}

void SceneRender::dragLatticeGizmo(Project* project, uint32_t sceneId, bool invertRotationSnap){
    ToolsLayer& tools = gizmoTools();
    RayReturn rretrun = mouseRay.intersects(cursorPlane);
    if (!rretrun){
        return;
    }

    const Matrix4 gizmoRMatrix = latticeDrag.rotation.getRotationMatrix();
    const Vector3 pivot = latticeDrag.pivot;
    const GizmoSelected gizmo = tools.getGizmoSelected();
    const GizmoSideSelected side = tools.getGizmoSideSelected();

    Matrix4 delta;
    if (gizmo == GizmoSelected::TRANSLATE && side != GizmoSideSelected::NONE){
        tools.mouseDrag(rretrun.point);
        Vector3 deltaPos = gizmoRMatrix.inverse() * ((rretrun.point + cursorStartOffset) - gizmoStartPosition);
        if (displaySettings.snapToGrid){
            float spacing = displaySettings.gridSpacing3D;
            if (spacing > 0.0f){
                deltaPos.x = std::round(deltaPos.x / spacing) * spacing;
                deltaPos.y = std::round(deltaPos.y / spacing) * spacing;
                deltaPos.z = std::round(deltaPos.z / spacing) * spacing;
            }
        }
        Vector3 axes;
        if (side == GizmoSideSelected::XYZ) axes = deltaPos;
        else if (side == GizmoSideSelected::X) axes = Vector3(deltaPos.x, 0, 0);
        else if (side == GizmoSideSelected::Y) axes = Vector3(0, deltaPos.y, 0);
        else if (side == GizmoSideSelected::Z) axes = Vector3(0, 0, deltaPos.z);
        else if (side == GizmoSideSelected::XY) axes = Vector3(deltaPos.x, deltaPos.y, 0);
        else if (side == GizmoSideSelected::XZ) axes = Vector3(deltaPos.x, 0, deltaPos.z);
        else if (side == GizmoSideSelected::YZ) axes = Vector3(0, deltaPos.y, deltaPos.z);
        delta = Matrix4::translateMatrix(gizmoRMatrix * axes);

    }else if (gizmo == GizmoSelected::ROTATE && side != GizmoSideSelected::NONE){
        tools.mouseDrag(rretrun.point);
        Vector3 lastPoint = pivot - rretrun.point;
        float dot = cursorStartOffset.dotProduct(lastPoint);
        float slength = cursorStartOffset.length() * lastPoint.length();
        float cosine = (slength != 0) ? dot / slength : 0;
        cosine = std::fmax(-1.0, std::fmin(1.0, cosine));
        float origAngle = std::acos(cosine);
        float sign = cursorStartOffset.crossProduct(lastPoint).dotProduct(cursorPlane.normal);
        float angle = snapRotationAngle((sign < 0) ? -origAngle : origAngle, invertRotationSnap);

        Quaternion rotation(Angle::radToDefault(angle), rotationAxis);
        delta = Matrix4::translateMatrix(pivot) * rotation.getRotationMatrix() * Matrix4::translateMatrix(-pivot);

    }else if (gizmo == GizmoSelected::SCALE && side != GizmoSideSelected::NONE){
        tools.mouseDrag(rretrun.point);
        Vector3 lastPoint = pivot - rretrun.point;
        Vector3 startRotPoint = gizmoRMatrix.inverse() * cursorStartOffset;
        Vector3 lastRotPoint = gizmoRMatrix.inverse() * lastPoint;
        float radioX = (startRotPoint.x != 0) ? (lastRotPoint.x / startRotPoint.x) : 1;
        float radioY = (startRotPoint.y != 0) ? (lastRotPoint.y / startRotPoint.y) : 1;
        float radioZ = (startRotPoint.z != 0) ? (lastRotPoint.z / startRotPoint.z) : 1;

        Vector3 newScale(1, 1, 1);
        if (side == GizmoSideSelected::XYZ){
            float startLength = cursorStartOffset.length();
            newScale = Vector3(startLength > 0.0f ? lastPoint.length() / startLength : 1.0f);
        }else if (side == GizmoSideSelected::X) newScale = Vector3(radioX, 1, 1);
        else if (side == GizmoSideSelected::Y) newScale = Vector3(1, radioY, 1);
        else if (side == GizmoSideSelected::Z) newScale = Vector3(1, 1, radioZ);
        else if (side == GizmoSideSelected::XY) newScale = Vector3(radioX, radioY, 1);
        else if (side == GizmoSideSelected::XZ) newScale = Vector3(radioX, 1, radioZ);
        else if (side == GizmoSideSelected::YZ) newScale = Vector3(1, radioY, radioZ);
        newScale = Vector3(std::fabs(newScale.x), std::fabs(newScale.y), std::fabs(newScale.z));

        delta = Matrix4::translateMatrix(pivot) * gizmoRMatrix * Matrix4::scaleMatrix(newScale) *
                gizmoRMatrix.inverse() * Matrix4::translateMatrix(-pivot);

    }else if (gizmo == GizmoSelected::OBJECT2D && tools.getGizmo2DSideSelected() == Gizmo2DSideSelected::CENTER){
        tools.mouseDrag(rretrun.point);
        // the points follow the cursor in the view plane
        Vector3 newPosition = rretrun.point + cursorStartOffset;
        if (displaySettings.snapToGrid){
            float spacing = displaySettings.gridSpacing2D;
            if (spacing > 0.0f){
                newPosition.x = std::round(newPosition.x / spacing) * spacing;
                newPosition.y = std::round(newPosition.y / spacing) * spacing;
            }
        }
        Vector3 move = newPosition - gizmoStartPosition;
        move.z = 0.0f;
        delta = Matrix4::translateMatrix(move);

    }else{
        return;
    }

    dragLatticePoints(project, sceneId, delta);
}

void SceneRender::dragLatticePoints(Project* project, uint32_t sceneId, const Matrix4& worldDelta){
    LatticeComponent* lattice = scene->findComponent<LatticeComponent>(latticeDrag.lattice);
    if (!latticeDrag.active || !lattice || lattice->resolutionX != latticeDrag.resolution[0] ||
        lattice->resolutionY != latticeDrag.resolution[1] || lattice->resolutionZ != latticeDrag.resolution[2] ||
        lattice->offsets.size() != latticeDrag.baseOffsets.size()){
        // the lattice changed under the drag (undo, resolution): it ends here
        endLatticeDrag();
        return;
    }

    // the gizmo's change in the lattice's local space, about the pivot there
    const Matrix4 inverse = latticeDrag.latticeModel.inverse();
    const Matrix4 local = inverse * worldDelta * latticeDrag.latticeModel;
    const Vector3 pivot = inverse * latticeDrag.pivot;
    const Matrix4 aboutPivot = Matrix4::translateMatrix(-pivot) * local * Matrix4::translateMatrix(pivot);

    std::vector<Vector3> result = LatticeEditorOps::transformedOffsets(latticeDrag.resolution[0], latticeDrag.resolution[1], latticeDrag.resolution[2],
        latticeDrag.baseOffsets, latticeDrag.points, aboutPivot, pivot, LatticeEditorOps::falloff());

    std::vector<int> changed;
    std::vector<Vector3> values;
    for (size_t i = 0; i < result.size(); i++){
        if (result[i] != lattice->offsets[i]){
            changed.push_back((int)i);
            values.push_back(result[i]);
        }
    }
    if (!changed.empty()){
        LatticeEditorOps::setOffsetsContinuous(project, sceneId, latticeDrag.lattice, changed, values);
    }
}

void SceneRender::endLatticeDrag(){
    if (latticeDrag.active){
        const uint32_t sceneId = getLatticeSceneId();
        if (sceneId != NULL_PROJECT_SCENE){
            LatticeEditorOps::endContinuousEdit(sceneId);
        }
    }
    latticeDrag = LatticeDrag();
}
