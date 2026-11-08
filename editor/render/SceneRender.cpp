// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "render/SceneRender2D.h"
#include "pixelart/PixelPerfectEditor.h"
#include "pixelart/PixelSnap.h"
#include "SceneRender.h"
#include "LayerViewCamera.h"
#include "component/AudioSourceComponent.h"
#include "component/MeshPolygonComponent.h"
#include "component/NavGridComponent.h"
#include "component/NavMeshAgentComponent.h"
#include "component/NavMeshLinkComponent.h"
#include "component/NavMeshModifierComponent.h"
#include "component/NavMeshObstacleComponent.h"
#include "component/NavMeshSurfaceComponent.h"
#include "component/SpriteComponent.h"
#include "component/TextComponent.h"
#include "subsystem/ActionSystem.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/UIDocumentSystem.h"
#include "subsystem/UISystem.h"
#include "object/Sprite.h"
#include "spline/SplineEditor.h"
#include "profiler/Profile.h"

#include "resources/icons/camera-icon_png.h"
#include "resources/icons/audio-source-icon_png.h"

// defined by resources/icons/bulb-icon_png.h, included in SceneRender3D.cpp
// (the xxd headers define non-static arrays, so they link in one TU only)
extern unsigned char bulb_icon_png[];
extern unsigned int bulb_icon_png_len;

#include "Backend.h"
#include "Project.h"
#include "navigation/NavigationEditor.h"
#include "imgui.h"
#include "command/CommandHandle.h"
#include "subsystem/TrailSystem.h"
#include "command/type/ObjectTransformCmd.h"
#include "command/type/PropertyCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "animation/ClipRecordHooks.h"
#include "lattice/LatticeEditorOps.h"
#include "util/GraphicUtils.h"
#include "window/PhysicsPlacementWindow.h"
#include "window/TerrainEditWindow.h"
#include "render/LineDrawUtils.h"
#include "render/Joint2DAnchors.h"
#include "util/Angle.h"
#include "util/FluidEditorTools.h"
#include "pose/PoseViewport.h"
#include "component/ParticleSystemComponent.h"
#include "component/TilemapComponent.h"
#include "particles/ParticleCompiler.h"
#include "subsystem/ParticleSubsystem.h"

#include <algorithm>
#include <cmath>
#include <cfloat>

using namespace jasperengine;

namespace {

    // The twelve edges of a box, by OBB corner
    const OBB::CornerEnum BOX_EDGES[12][2] = {
        {OBB::NEAR_LEFT_BOTTOM, OBB::NEAR_LEFT_TOP}, {OBB::NEAR_LEFT_TOP, OBB::NEAR_RIGHT_TOP},
        {OBB::NEAR_RIGHT_TOP, OBB::NEAR_RIGHT_BOTTOM}, {OBB::NEAR_RIGHT_BOTTOM, OBB::NEAR_LEFT_BOTTOM},
        {OBB::FAR_LEFT_BOTTOM, OBB::FAR_LEFT_TOP}, {OBB::FAR_LEFT_TOP, OBB::FAR_RIGHT_TOP},
        {OBB::FAR_RIGHT_TOP, OBB::FAR_RIGHT_BOTTOM}, {OBB::FAR_RIGHT_BOTTOM, OBB::FAR_LEFT_BOTTOM},
        {OBB::FAR_LEFT_BOTTOM, OBB::NEAR_LEFT_BOTTOM}, {OBB::FAR_LEFT_TOP, OBB::NEAR_LEFT_TOP},
        {OBB::FAR_RIGHT_TOP, OBB::NEAR_RIGHT_TOP}, {OBB::FAR_RIGHT_BOTTOM, OBB::NEAR_RIGHT_BOTTOM}
    };

}

bool editor::SceneRender::isGizmoGone(const EntityHandle* gizmo, const RenderSystem& render) const{
    if (!gizmo){
        return false;
    }
    Scene* gizmoScene = gizmo->getScene();
    const Entity entity = gizmo->getEntity();
    if (!gizmoScene->isEntityCreated(entity) || !gizmoScene->findComponent<Transform>(entity)){
        return true;
    }
    // the tools layers' gizmos carry no mark
    return gizmoScene == scene && !render.isEditorOverlay(entity);
}

void editor::SceneRender::deleteGizmo(EntityHandle* gizmo, const RenderSystem& render) const{
    if (gizmo && isGizmoGone(gizmo, render)){
        gizmo->setEntityOwned(false);
    }
    delete gizmo;
}

void editor::SceneRender::dropGoneGizmos(){
    const RenderSystem& render = *scene->getSystem<RenderSystem>();
    dropGoneGizmos(latticeCages, render, &LatticeCageLines::overlay, &LatticeCageLines::depth);
    dropGoneGizmos(particleShapeLines, render);
}

void editor::SceneRender::ensureOverlayLines(std::map<Entity, LineRenderer*>& lines, Entity entity){
    if (lines.find(entity) == lines.end()){
        ScopedDefaultEntityPool sys(*scene, EntityPool::System);
        lines[entity] = overlay(new LineRenderer(scene));
    }
}

// the selected Particle System's shape in the 2D Scene view (the 3D view's are in its particle
// lines, SceneRender3D): the effect's Shape module outline (ParticleCompiler::shapeOutline)
void editor::SceneRender::updateParticleShapeLines(Entity entity, bool selected, std::set<Entity>& current){
    const ParticleSystemComponent* ps = scene->findComponent<ParticleSystemComponent>(entity);
    const Transform* transform = scene->findComponent<Transform>(entity);
    if (!ps || !transform) return;
    current.insert(entity);
    ensureOverlayLines(particleShapeLines, entity);
    LineRenderer* lines = particleShapeLines[entity];
    lines->clearLines();
    const particles::ParticleEffectDocument* document = ParticleSubsystem::getDocument(*ps);
    const bool visible = selected && transform->visible && document && document->shape.present && document->shape.enabled;
    lines->setVisible(visible);
    if (!visible) return;
    std::vector<Vector3> segments;
    particles::ParticleCompiler::shapeOutline(document->shape, segments);
    const Vector4 color(0.45f, 0.8f, 1.0f, 1.0f);
    for (size_t i = 0; i + 1 < segments.size(); i += 2){
        lines->addLine(transform->modelMatrix * segments[i], transform->modelMatrix * segments[i + 1], color);
    }
}

void editor::SceneRender::dropParticleShapeLines(const std::set<Entity>& current){
    for (auto it = particleShapeLines.begin(); it != particleShapeLines.end();){
        if (current.find(it->first) == current.end()){
            delete it->second;
            it = particleShapeLines.erase(it);
        }else{
            ++it;
        }
    }
}

void editor::SceneRender::setEditorOverlay(Entity entity){
    scene->getSystem<RenderSystem>()->setEditorOverlay(entity, true);
}

editor::SceneRender::SceneRender(Scene* scene, bool use2DGizmos, bool enable3DOverlays, float gizmoScale, float selectionOffset): toolslayer(use2DGizmos), uilayer(enable3DOverlays){
    ScopedDefaultEntityPool sys(*scene, EntityPool::System);

    this->mouseClicked = false;
    this->useGlobalTransform = true;
    this->isPlaying = false;

    this->gizmoScale = gizmoScale;
    this->selectionOffset = selectionOffset;

    this->scene = scene;
    this->camera = new Camera(scene);
    this->previewCameraEntity = NULL_ENTITY;

    this->multipleEntitiesSelected = false;
    this->zoom = 1.0f;

    selLines = overlay(new LineRenderer(scene));
    selLines->addLine(Vector3::ZERO, Vector3::ZERO, Vector4::ZERO);
    selLines->setVisible(false);

    terrainBrushLines = overlay(new LineRenderer(scene));
    terrainBrushLines->addLine(Vector3::ZERO, Vector3::ZERO, Vector4::ZERO);
    terrainBrushLines->setVisible(false);

    physicsLines = overlay(new LineRenderer(scene));
    physicsLines->setMaxLines(1024);
    physicsLines->addLine(Vector3::ZERO, Vector3::ZERO, Vector4::ZERO);
    physicsLines->setVisible(false);

    scene->setCamera(camera);

    cursorSelected = CursorSelected::POINTER;
}

editor::SceneRender::~SceneRender(){
    framebuffer.destroy();

    for (int i = 0; i < 2; i++){
        if (!layerTools[i]){
            continue;
        }
        if (Engine::isSceneRunning(layerTools[i]->getScene())){
            Engine::removeScene(layerTools[i]->getScene());
        }
        delete layerSelLines[i];
        layerSelLines[i] = nullptr;
    }

    destroyLatticeCages();
    navigationOverlay.reset();
    destroySplineOverlay();
    delete wireframeLines;
    for (auto& pair : particleShapeLines) {
        delete pair.second;
    }
    delete terrainBrushLines;
    delete physicsLines;
    delete camera;
}

AABB editor::SceneRender::getMeshLocalAABB(const MeshComponent& mesh){
    return mesh.skinnedAABB.isNull() ? mesh.aabb : mesh.skinnedAABB;
}

AABB editor::SceneRender::getAABB(Scene* target, Entity entity, bool local){
    Signature signature = target->getSignature(entity);
    if (signature.test(target->getComponentId<MeshComponent>())){
        MeshComponent& mesh = target->getComponent<MeshComponent>(entity);
        if (local){
            return getMeshLocalAABB(mesh);
        }else{
            return mesh.worldAABB;
        }
    }else if (signature.test(target->getComponentId<UIComponent>())){
        if (!signature.test(target->getComponentId<PolygonComponent>()) && signature.test(target->getComponentId<RectTransformComponent>())){
            RectTransformComponent& layout = target->getComponent<RectTransformComponent>(entity);
            if (layout.width > 0 && layout.height > 0){
                Vector2 center = GraphicUtils::getRectTransformCenter(target, entity, layout);
                AABB aabb = AABB(-center.x, -center.y, 0, layout.width-center.x, layout.height-center.y, 0);
                if (local){
                    return aabb;
                }else{
                    if (signature.test(target->getComponentId<Transform>())){
                        Transform& transform = target->getComponent<Transform>(entity);
                        return transform.modelMatrix * aabb;
                    }
                    return aabb;
                }
            }
        }

        UIComponent& ui = target->getComponent<UIComponent>(entity);
        if (local){
            return ui.aabb;
        }else{
            return ui.worldAABB;
        }
    }else if (signature.test(target->getComponentId<RectTransformComponent>())){
        RectTransformComponent& layout = target->getComponent<RectTransformComponent>(entity);
        if (layout.width > 0 && layout.height > 0){
            Vector2 center = GraphicUtils::getRectTransformCenter(target, entity, layout);
            AABB aabb = AABB(-center.x, -center.y, 0, layout.width-center.x, layout.height-center.y, 0);
            if (local){
                return aabb;
            }else{
                if (signature.test(target->getComponentId<Transform>())){
                    Transform& transform = target->getComponent<Transform>(entity);
                    return transform.modelMatrix * aabb;
                }
                return aabb;
            }
        }
    }else if ((signature.test(target->getComponentId<CameraComponent>()) ||
               signature.test(target->getComponentId<LightComponent>()) ||
               signature.test(target->getComponentId<Light2DComponent>()) ||
               signature.test(target->getComponentId<AudioSourceComponent>())) && signature.test(target->getComponentId<Transform>())){
        Transform& transform = target->getComponent<Transform>(entity);
        float halfSize = targetBillboardScale(target, transform.worldPosition, 16.0f);
        AABB aabb = isOrthographicView(target)
            ? AABB(-halfSize, -halfSize, -1, halfSize, halfSize, 1)
            : AABB(-halfSize, -halfSize, -halfSize, halfSize, halfSize, halfSize);

        if (local){
            return aabb;
        }else{
            return transform.modelMatrix * aabb;
        }
    }else if (signature.test(target->getComponentId<ShadowCaster2DComponent>()) && signature.test(target->getComponentId<Transform>())){
        ShadowCaster2DComponent& occluder = target->getComponent<ShadowCaster2DComponent>(entity);
        Transform& transform = target->getComponent<Transform>(entity);

        // hug the polygon points; the entity origin is only the fallback when
        // there is nothing else to bound
        AABB aabb(Vector3(0, 0, -1), Vector3(0, 0, 1));
        if (occluder.shape == ShadowCaster2DShape::POLYGON && occluder.points.size() > 0){
            aabb.setNull();
            for (const Vector2& point : occluder.points){
                aabb.merge(Vector3(point.x, point.y, 0.0f));
            }
            Vector3 mn = aabb.getMinimum(); mn.z = -1.0f;
            Vector3 mx = aabb.getMaximum(); mx.z = 1.0f;
            aabb.setExtents(mn, mx);
        }

        if (local){
            return aabb;
        }else{
            return transform.modelMatrix * aabb;
        }
    }else if ((signature.test(target->getComponentId<PointsComponent>()) || signature.test(target->getComponentId<LineRendererComponent>())) && signature.test(target->getComponentId<Transform>())){
        Transform& transform = target->getComponent<Transform>(entity);
        AABB aabb(Vector3::ZERO, Vector3::ZERO);

        if (signature.test(target->getComponentId<PointsComponent>())){
            PointsComponent& points = target->getComponent<PointsComponent>(entity);
            for (const PointData& pt : points.points){
                if (pt.visible){
                    aabb.merge(pt.position);
                }
            }
        }

        if (signature.test(target->getComponentId<LineRendererComponent>())){
            mergeLineLocalPoints(aabb, target->getComponent<LineRendererComponent>(entity), transform);
        }

        if (local){
            return aabb;
        }else{
            return transform.modelMatrix * aabb;
        }
    }

    return AABB();
}

AABB editor::SceneRender::getFamilyAABB(Scene* target, Entity entity, float offset){
    Signature signature = target->getSignature(entity);
    if (!signature.test(target->getComponentId<Transform>())) {
        AABB entityAABB = getAABB(target, entity, false);
        if (!entityAABB.isNull() && !entityAABB.isInfinite()) {
            Vector3 min = entityAABB.getMinimum() - Vector3(offset);
            Vector3 max = entityAABB.getMaximum() + Vector3(offset);
            entityAABB.setExtents(min, max);
        }
        return entityAABB;
    }

    auto transforms = target->getComponentArray<Transform>();
    size_t index = transforms->getIndex(entity);

    AABB aabb;
    std::vector<Entity> parentList;
    for (int i = index; i < transforms->size(); i++){
        Transform& transform = transforms->getComponentFromIndex(i);

        // Finding childs
        if (i > index){
            if (std::find(parentList.begin(), parentList.end(), transform.parent) == parentList.end()){
                break;
            }
        }

        entity = transforms->getEntity(i);
        parentList.push_back(entity);

        AABB entityAABB = getAABB(target, entity, false);

        if (!entityAABB.isNull() && !entityAABB.isInfinite()){
            Vector3 min = entityAABB.getMinimum() - Vector3(offset);
            Vector3 max = entityAABB.getMaximum() + Vector3(offset);
            entityAABB.setExtents(min, max);

            aabb.merge(entityAABB);
        }
    }

    return aabb;
}

AABB editor::SceneRender::getEntitiesAABB(const std::vector<Entity>& entities){
    AABB aabb;
    for (Entity entity : entities) {
        AABB entityAABB = getFamilyAABB(scene, entity, 0.0f);
        if (!entityAABB.isNull() && !entityAABB.isInfinite()) {
            aabb.merge(entityAABB);
        }
    }
    return aabb;
}

// Transforms a local AABB into a world-space box using the FULL model matrix,
// preserving shear. A node's world matrix can be sheared (e.g. a non-uniform
// parent scale combined with the child's rotation), which a regular OBB cannot
// represent: Matrix4*OBB decomposes the matrix and discards the shear, so the
// resulting box is rotated/mis-sized relative to the rendered geometry.
//
// The engine OBB must stay orthonormal for SAT collision/enclose, so this is
// kept editor-side: for the selection outline and gizmo bounds we want a box
// that hugs the actual geometry, i.e. a parallelepiped. We map the half-extent
// edge vectors through the linear part and store them as (normalized) axes plus
// their lengths, so OBB::getCorner reproduces the transformed box exactly.
OBB editor::SceneRender::transformAABBPreservingShear(const Matrix4& modelMatrix, const AABB& localAABB){
    OBB local = localAABB.getOBB();
    if (local.isNull() || local.isInfinite()){
        return modelMatrix * local;
    }

    Vector3 he = local.getHalfExtents();
    Matrix3 linear = modelMatrix.linear();

    Vector3 edgeX = linear * Vector3(he.x, 0.0f, 0.0f);
    Vector3 edgeY = linear * Vector3(0.0f, he.y, 0.0f);
    Vector3 edgeZ = linear * Vector3(0.0f, 0.0f, he.z);

    // Half-extents are the transformed edge lengths; a degenerate (zero-extent)
    // axis must keep its 0 length so a flat box stays flat.
    Vector3 halfExtents(edgeX.length(), edgeY.length(), edgeZ.length());

    // A flat box leaves that axis direction undefined; substitute a valid
    // orthogonal direction so the gizmo orientation stays well-defined. The
    // zero half-extent above means it never affects the drawn corners.
    if (halfExtents.x < 1e-6f) edgeX = edgeY.crossProduct(edgeZ);
    if (halfExtents.y < 1e-6f) edgeY = edgeZ.crossProduct(edgeX);
    if (halfExtents.z < 1e-6f) edgeZ = edgeX.crossProduct(edgeY);

    OBB result(modelMatrix * local.getCenter(), halfExtents);
    result.setAxes(edgeX, edgeY, edgeZ);
    return result;
}

OBB editor::SceneRender::getOBB(Scene* target, Entity entity, bool local, bool visual){
    Signature signature = target->getSignature(entity);

    Matrix4 modelMatrix;
    if (signature.test(target->getComponentId<Transform>())){
        Transform& transform = target->getComponent<Transform>(entity);
        modelMatrix = transform.modelMatrix;

        if (signature.test(target->getComponentId<MeshComponent>())){
            MeshComponent& mesh = target->getComponent<MeshComponent>(entity);
            AABB meshAABB = getMeshLocalAABB(mesh);
            if (local){
                return meshAABB.getOBB();
            }else{
                return visual ? transformAABBPreservingShear(modelMatrix, meshAABB)
                              : modelMatrix * meshAABB.getOBB();
            }
        }else if (signature.test(target->getComponentId<UIComponent>())){
            if (!signature.test(target->getComponentId<PolygonComponent>()) && signature.test(target->getComponentId<RectTransformComponent>())){
                RectTransformComponent& layout = target->getComponent<RectTransformComponent>(entity);
                if (layout.width > 0 && layout.height > 0){
                    Vector2 center = GraphicUtils::getRectTransformCenter(target, entity, layout);
                    AABB aabb = AABB(-center.x, -center.y, 0, layout.width-center.x, layout.height-center.y, 0);
                    if (local){
                        return aabb.getOBB();
                    }else{
                        return visual ? transformAABBPreservingShear(modelMatrix, aabb)
                              : modelMatrix * aabb.getOBB();
                    }
                }
            }

            UIComponent& ui = target->getComponent<UIComponent>(entity);
            if (local){
                return ui.aabb.getOBB();
            }else{
                return visual ? transformAABBPreservingShear(modelMatrix, ui.aabb)
                              : modelMatrix * ui.aabb.getOBB();
            }
        }else if (signature.test(target->getComponentId<RectTransformComponent>())){
            RectTransformComponent& layout = target->getComponent<RectTransformComponent>(entity);
            if (layout.width > 0 && layout.height > 0){
                Vector2 center = GraphicUtils::getRectTransformCenter(target, entity, layout);
                AABB aabb = AABB(-center.x, -center.y, 0, layout.width-center.x, layout.height-center.y, 0);
                if (local){
                    return aabb.getOBB();
                }else{
                    return visual ? transformAABBPreservingShear(modelMatrix, aabb)
                              : modelMatrix * aabb.getOBB();
                }
            }
        }else if (signature.test(target->getComponentId<CameraComponent>()) ||
                   signature.test(target->getComponentId<LightComponent>()) ||
                   signature.test(target->getComponentId<Light2DComponent>()) ||
                   signature.test(target->getComponentId<AudioSourceComponent>())){
            float halfSize = targetBillboardScale(target, transform.worldPosition, 16.0f);
            AABB aabb = isOrthographicView(target)
                ? AABB(-halfSize, -halfSize, -1, halfSize, halfSize, 1)
                : AABB(-halfSize, -halfSize, -halfSize, halfSize, halfSize, halfSize);
            if (local){
                return aabb.getOBB();
            }else{
                return visual ? transformAABBPreservingShear(modelMatrix, aabb)
                              : modelMatrix * aabb.getOBB();
            }
        }else if (signature.test(target->getComponentId<ShadowCaster2DComponent>())){
            ShadowCaster2DComponent& occluder = target->getComponent<ShadowCaster2DComponent>(entity);

            // hug the polygon points; the entity origin is only the fallback when
            // there is nothing else to bound
            AABB aabb(Vector3(0, 0, -1), Vector3(0, 0, 1));
            if (occluder.shape == ShadowCaster2DShape::POLYGON && occluder.points.size() > 0){
                aabb.setNull();
                for (const Vector2& point : occluder.points){
                    aabb.merge(Vector3(point.x, point.y, 0.0f));
                }
                Vector3 mn = aabb.getMinimum(); mn.z = -1.0f;
                Vector3 mx = aabb.getMaximum(); mx.z = 1.0f;
                aabb.setExtents(mn, mx);
            }

            if (local){
                return aabb.getOBB();
            }else{
                return visual ? transformAABBPreservingShear(modelMatrix, aabb)
                          : modelMatrix * aabb.getOBB();
            }
        }else if (signature.test(target->getComponentId<PointsComponent>()) || signature.test(target->getComponentId<LineRendererComponent>())){
            AABB aabb(Vector3::ZERO, Vector3::ZERO);

            if (signature.test(target->getComponentId<PointsComponent>())){
                PointsComponent& points = target->getComponent<PointsComponent>(entity);
                for (const PointData& pt : points.points){
                    if (pt.visible){
                        aabb.merge(pt.position);
                    }
                }
            }

            if (signature.test(target->getComponentId<LineRendererComponent>())){
                mergeLineLocalPoints(aabb, target->getComponent<LineRendererComponent>(entity), transform);
            }

            if (local){
                return aabb.getOBB();
            }else{
                return visual ? transformAABBPreservingShear(modelMatrix, aabb)
                              : modelMatrix * aabb.getOBB();
            }
        }

        if (local){
            return OBB::ZERO;
        }else{
            return modelMatrix * OBB::ZERO;
        }
    }

    return OBB(); // null OBB
}

// Builds a single shear-preserving selection box enclosing the entity and every
// entity in its subtree. We cannot merge the members with OBB::enclose: the visual
// boxes have non-orthonormal axes (preserved shear), and enclose projects onto
// those axes assuming they are orthonormal, which inflates and skews the result.
// Instead we gather every member's world-space corners in the SELECTED entity's
// local frame, build one local AABB, then transform it once with the selected
// entity's matrix. The box is oriented to the selected node and tightly bounds the
// whole subtree (and reduces to the exact single-mesh box for a leaf). A singular
// selected transform falls back to an axis-aligned world-space box.
OBB editor::SceneRender::getFamilyVisualOBB(Scene* target, Entity entity, float offset){
    Signature signature = target->getSignature(entity);
    if (!signature.test(target->getComponentId<Transform>())) {
        OBB entityOBB = getOBB(target, entity, false, /*visual*/ true);
        if (!entityOBB.isNull()){
            entityOBB.setHalfExtents(entityOBB.getHalfExtents() + Vector3(offset));
        }
        return entityOBB;
    }

    const Matrix4& selectedMatrix = target->getComponent<Transform>(entity).modelMatrix;
    const float selectedDeterminant = selectedMatrix.linear().determinant();
    const bool useSelectedFrame = selectedMatrix.isValid()
        && std::isfinite(selectedDeterminant)
        && selectedDeterminant != 0.0f;

    const Matrix4 worldToBounds = useSelectedFrame ? selectedMatrix.inverse() : Matrix4();

    auto transforms = target->getComponentArray<Transform>();
    size_t index = transforms->getIndex(entity);

    AABB familyBounds;
    std::vector<Entity> parentList;
    for (int i = index; i < transforms->size(); i++){
        Transform& transform = transforms->getComponentFromIndex(i);

        // Finding childs
        if (i > index){
            if (std::find(parentList.begin(), parentList.end(), transform.parent) == parentList.end()){
                break;
            }
        }

        Entity member = transforms->getEntity(i);
        parentList.push_back(member);

        OBB memberOBB = getOBB(target, member, false, /*visual*/ true);
        if (memberOBB.isNull()){
            continue;
        }

        const Vector3* corners = memberOBB.getCorners();
        for (int c = 0; c < 8; c++){
            const Vector3 point = worldToBounds * corners[c];
            if (point.isValid()){
                familyBounds.merge(point);
            }
        }
    }

    if (familyBounds.isNull()){
        return OBB();
    }

    OBB result = useSelectedFrame
        ? transformAABBPreservingShear(selectedMatrix, familyBounds)
        : familyBounds.getOBB();
    result.setHalfExtents(result.getHalfExtents() + Vector3(offset));
    return result;
}

void editor::SceneRender::hideEntityGizmos(Entity entity){
    auto shape = particleShapeLines.find(entity);
    if (shape != particleShapeLines.end()) shape->second->setVisible(false);
}

bool editor::SceneRender::gizmoActive(Entity entity) const{
    return !scene->hasInactiveEntities() || scene->isEntityActiveInHierarchy(entity);
}

void editor::SceneRender::hideAllGizmos(){
    // (also the subclasses' gizmos: they hide theirs after calling this)
    dropGoneGizmos();

    selLines->setVisible(false);
    terrainBrushLines->setVisible(false);
    physicsLines->setVisible(false);
    toolslayer.setGizmoVisible(false);
    for (int i = 0; i < 2; i++){
        if (layerTools[i]){
            layerTools[i]->setGizmoVisible(false);
            layerSelLines[i]->setVisible(false);
        }
    }
    uilayer.setViewGizmoImageVisible(false);
    uilayer.setSelectionBoxVisible(false);
    uilayer.setCameraGaugeVisible(false);
    hideLatticeCages();
    if (navigationOverlay) navigationOverlay->hide();
    hideSplineOverlay();
}

void editor::SceneRender::setPlayMode(bool isPlaying){
    this->isPlaying = isPlaying;
    if (isPlaying){
        // Play mode owns the scene camera (the game main camera), so drop any editor
        // camera preview. This keeps the invariant "playing => no preview" and avoids
        // dropping the user back into preview view when the scene is stopped. The
        // gizmos stay: the Scene view keeps drawing them, the game never does.
        previewCameraEntity = NULL_ENTITY;
    }else{
        syncSceneCamera();
    }
}

void editor::SceneRender::activate(){
    if (!isPlaying){
        syncSceneCamera();
    }

    Engine::setFramebuffer(&framebuffer);
    scene->getSystem<RenderSystem>()->setDrawEditorOverlays(true);

    if (underlayScene){
        // Prefab Mode In Context: the context scene clears and draws first, through the
        // camera that follows this view, then this scene over it (a layer keeps the colour)
        underlayScene->setCamera(underlayCamera);
        underlayScene->getSystem<RenderSystem>()->setDrawEditorOverlays(false);
        Engine::setScene(underlayScene);
        Engine::removeAllSceneLayers(false);
        if (underlayVeil){
            Engine::addSceneLayer(underlayVeil);
        }
        Engine::addSceneLayer(scene);
    }else{
        Engine::setScene(scene);
        Engine::removeAllSceneLayers(false);
    }

    for (Scene* childScene : childSceneLayers) {
        // inline child scenes show their gizmos and icons in the Scene view (a Game view
        // pass turned them off)
        childScene->getSystem<RenderSystem>()->setDrawEditorOverlays(true);
        Engine::addSceneLayer(childScene);
    }

    Engine::addSceneLayer(toolslayer.getScene());
    engineLayerTools = editLayer.active ? layerToolsIndex() : -1;
    if (engineLayerTools >= 0){
        Engine::addSceneLayer(layerTools[engineLayerTools]->getScene());
    }
    Engine::addSceneLayer(uilayer.getScene());
    engineLayersActive = true;
}

void editor::SceneRender::activateGame(Framebuffer* target){
    Engine::setFramebuffer(target);
    Engine::setScene(scene);
    // the game never shows the gizmos and icons living in its scene, nor the Scene view's
    // debug toggles (update() may not run before this frame: the Scene view can be hidden)
    scene->getSystem<RenderSystem>()->setDrawEditorOverlays(false);
    scene->getSystem<RenderSystem>()->setDisableFaceCulling(false);
    scene->getSystem<RenderSystem>()->setDisableFog(false);
    // and draws its post-processing: the edit-mode Scene view leaves it off unless it shows it
    scene->getSystem<RenderSystem>()->setDisablePostProcessing(false);

    Engine::removeAllSceneLayers(false);
    engineLayersActive = false;
    engineLayerTools = -1;
}

void editor::SceneRender::addOverlayLayers(std::vector<Engine::ViewLayer>& layers, int width, int height){
    const Rect full(0, 0, (float)width, (float)height);

    Scene* toolsScene = toolslayer.getScene();
    toolsScene->getSystem<RenderSystem>()->updateCameraSizeForCanvas(toolsScene->getCamera(), (float)width, (float)height);
    toolsScene->update(Engine::getDeltatime());
    layers.push_back({toolsScene, toolsScene->getCamera(), full, false, false});

    // the edit layer's gizmos, through its camera made to cover the view
    if (editLayer.active){
        Scene* layerToolsScene = layerTools[layerToolsIndex()]->getScene();
        layerToolsScene->update(Engine::getDeltatime());
        layers.push_back({layerToolsScene, layerToolsScene->getCamera(), full, false, false});
    }

    Scene* uiScene = uilayer.getScene();
    uiScene->getSystem<RenderSystem>()->updateCameraSizeForCanvas(uiScene->getCamera(), (float)width, (float)height);
    uiScene->update(Engine::getDeltatime());
    layers.push_back({uiScene, uiScene->getCamera(), full, false, false});
}

void editor::SceneRender::drawOverlayTargets(){
}

void editor::SceneRender::drawSceneView(int width, int height, const std::vector<Scene*>& layerScenes, bool gameLayers,
        Scene* replacingBase, Entity replacingCamera){
    if (width <= 0 || height <= 0){
        return;
    }

    if (!framebuffer.isCreated() || framebuffer.getWidth() != (unsigned int)width || framebuffer.getHeight() != (unsigned int)height){
        framebuffer.destroy();
        framebuffer.setWidth((unsigned int)width);
        framebuffer.setHeight((unsigned int)height);
        JE_GPU_ALLOC_SCOPE(Editor, "Scene view");
        framebuffer.create();
    }
    if (viewWidth != width || viewHeight != height){
        updateSize(width, height);
    }

    // Gizmos and icons placed after the scene's update: their transforms are settled
    // here for this draw (the scene's next update takes them from there)
    settleOverlayTransforms();

    const Rect full(0, 0, (float)width, (float)height);
    const bool previewing = !isPlaying && isPreviewCameraActive();
    const Entity viewCamera = previewing ? previewCameraEntity : camera->getEntity();

    std::vector<Engine::ViewLayer> layers;
    // Prefab Mode In Context: the context scene under this one, as activate() sets it up
    if (underlayScene && !gameLayers){
        underlayScene->update(Engine::getDeltatime());
        layers.push_back({underlayScene, underlayCamera, full, false, false, false, (float)width, (float)height});
        if (underlayVeil){
            underlayVeil->update(Engine::getDeltatime());
            layers.push_back({underlayVeil, underlayVeil->getCamera(), full, false, false});
        }
    }
    // the Scene view's own fog and face culling toggles hold while playing too, and in this
    // view only (the Game view and the game keep the scene's culling); a previewed scene
    // camera covers the view as the Engine's native canvas would size it
    // after a game load replaced this scene, the level it runs now, through the editor camera
    const bool replaced = replacingBase && replacingCamera != NULL_ENTITY;
    layers.push_back({replaced ? replacingBase : scene, replaced ? replacingCamera : viewCamera, full, !previewing, false,
                      displaySettings.disableFog, (float)width, (float)height,
                      displaySettings.disableFaceCulling, displaySettings.showPostProcessing || previewing, displaySettings.disableClouds});
    layers.back().drawMode = previewing ? 0 : (uint8_t)displaySettings.drawMode;
    // S41 R21: Snap Preview, the drawables snapped on this view's own grid of art pixels (a scene
    // without a snapping Pixel Perfect Camera has no snapped variants: nothing changes)
    if (!previewing && displaySettings.pixelSnapPreview && zoom > 0.0f && dynamic_cast<SceneRender2D*>(this))
        layers.back().snapPreviewScale = 1.0f / (zoom * (float)Engine::getPixelsPerUnit());

    if (gameLayers){
        // The game's layer scenes (a HUD) cover its canvas, framed in this view at the
        // canvas aspect as the editor frames a child scene over the scene
        const Rect gameRect = getGameLayerRect(width, height);
        for (Scene* gameLayer : layerScenes){
            if (gameLayer && gameLayer != scene && gameLayer != replacingBase){
                layers.push_back({gameLayer, gameLayer->getCamera(), gameRect, false, false});
            }
        }
    }else{
        // inline child scenes cover the view through their own cameras, as in activate()
        for (Scene* childScene : layerScenes){
            if (childScene && childScene != scene){
                layers.push_back({childScene, childScene->getCamera(), full, false, false, false, (float)width, (float)height});
            }
        }
    }

    if (!previewing){
        drawOverlayTargets();
    }
    addOverlayLayers(layers, width, height);

    Engine::drawView(SCENE_VIEW_ID, &framebuffer, layers);
}

void editor::SceneRender::settleOverlayTransforms(){
    RenderSystem* renderSystem = scene->getSystem<RenderSystem>().get();
    auto transforms = scene->getComponentArray<Transform>();
    for (size_t i = 0; i < transforms->size(); i++){
        Transform& transform = transforms->getComponentFromIndex(i);
        const Entity entity = transforms->getEntity(i);
        if (!transform.needUpdate || !renderSystem->isEditorOverlay(entity)){
            continue;
        }
        renderSystem->updateTransform(transform, entity);
        if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)){
            mesh->worldAABB = transform.modelMatrix * (mesh->skinnedAABB.isNull() ? mesh->aabb : mesh->skinnedAABB);
        }
    }
}

void editor::SceneRender::drawPaneView(uint32_t viewId, Entity paneCamera, Framebuffer* target, int width, int height, const PaneViewFlags& flags){
    if (width <= 0 || height <= 0 || !target || paneCamera == NULL_ENTITY){
        return;
    }
    if (!target->isCreated() || target->getWidth() != (unsigned int)width || target->getHeight() != (unsigned int)height){
        target->destroy();
        target->setWidth((unsigned int)width);
        target->setHeight((unsigned int)height);
        JE_GPU_ALLOC_SCOPE(Editor, "Scene view pane");
        target->create();
    }

    // the pane's camera as just placed (this frame's update ran before it moved)
    if (Transform* cameraTransform = scene->findComponent<Transform>(paneCamera); cameraTransform && cameraTransform->needUpdate){
        scene->getSystem<RenderSystem>()->updateTransform(*cameraTransform, paneCamera);
    }

    // the grid in the pane's axis plane for this draw only
    const int gridPlane = getGridPlane();
    setGridPlane(flags.gridPlane);
    settleOverlayTransforms();

    const Rect full(0, 0, (float)width, (float)height);
    std::vector<Engine::ViewLayer> layers;
    Engine::ViewLayer base;
    base.scene = scene;
    base.camera = paneCamera;
    base.viewRect = full;
    base.editorOverlays = true;
    base.disableFog = displaySettings.disableFog;
    base.canvasWidth = (float)width;
    base.canvasHeight = (float)height;
    base.disableFaceCulling = displaySettings.disableFaceCulling;
    base.postProcessing = flags.postProcessing;
    base.disableClouds = displaySettings.disableClouds;
    base.shadows = flags.shadows;
    base.drawMode = (uint8_t)displaySettings.drawMode;
    layers.push_back(base);
    for (Scene* childScene : childSceneLayers){
        if (childScene && childScene != scene){
            layers.push_back({childScene, childScene->getCamera(), full, false, false, false, (float)width, (float)height});
        }
    }
    Engine::drawView(viewId, target, layers);

    setGridPlane(gridPlane);
    settleOverlayTransforms();
}

void editor::SceneRender::setChildSceneLayers(const std::vector<Scene*>& layers){
    childSceneLayers = layers;
}

bool editor::SceneRender::setUnderlay(Scene* underlay, Entity camera, Scene* veil){
    if (!underlay){
        camera = NULL_ENTITY;
        veil = nullptr;
    }
    if (underlayScene == underlay && underlayCamera == camera && underlayVeil == veil){
        return false;
    }
    underlayScene = underlay;
    underlayCamera = camera;
    underlayVeil = veil;
    return true;
}

void editor::SceneRender::updateSize(int width, int height){
    viewWidth = width;
    viewHeight = height;

    // an auto-resize editor camera follows the Scene view, which is not the Engine's
    // view while the game plays
    scene->getSystem<RenderSystem>()->updateCameraSizeForCanvas(camera->getEntity(), (float)width, (float)height);

    // the overlays anchor to the Scene view, not to the Engine canvas (the game's while playing)
    uilayer.getScene()->getSystem<UISystem>()->setAnchorReferenceSize(width, height);
}

void editor::SceneRender::setOverlayScale(float scale){
    if (scale <= 0.0f){
        scale = 1.0f;
    }
    if (overlayScale == scale){
        return;
    }
    overlayScale = scale;
    uilayer.setOverlayScale(scale);
}

float editor::SceneRender::billboardScreenScale(const Vector3& worldPos, float logicalScale) const {
    CameraComponent& cameracomp = scene->getComponent<CameraComponent>(camera->getEntity());
    if (cameracomp.type == CameraType::CAMERA_PERSPECTIVE){
        float dist = (worldPos - camera->getWorldPosition()).length();
        float s = std::tan(cameracomp.yfov) * dist * (overlayPx(logicalScale) / (float)framebuffer.getHeight());
        if (!std::isfinite(s) || s <= 0.0f) {
            return 1.0f;
        }
        return s;
    }
    return overlayPx(logicalScale) * zoom;
}

void editor::SceneRender::updateRenderSystem(){
    if (!isPlaying){
        syncSceneCamera();
    }

    // Meshes and UIs are created in update, without this can affect worldAABB
    scene->getSystem<MeshSystem>()->update(0);
    scene->getSystem<UISystem>()->update(0);
    scene->getSystem<UIDocumentSystem>()->update(0);
    // to avoid gizmos delays
    scene->getSystem<RenderSystem>()->update(0);
}

void editor::SceneRender::update(std::vector<Entity> selEntities, std::vector<Entity> entities, Entity mainCamera, const SceneDisplaySettings& settings){
    // (also the subclasses' gizmos: their update runs after this one)
    dropGoneGizmos();

    // a line point stays selected only while its GameObject is selected and its line keeps the
    // mode (the index means a polyline position or a segment end) and the point
    if (selectedLinePointIndex >= 0){
        const LineRendererComponent* lines = scene->findComponent<LineRendererComponent>(selectedLinePointEntity);
        const size_t count = lines ? (lines->mode == LineRendererMode::Polyline ? lines->positions.size() : lines->lines.size() * 2) : 0;
        if (!lines || lines->mode != selectedLinePointMode || selectedLinePointIndex >= (int)count ||
                std::find(selEntities.begin(), selEntities.end(), selectedLinePointEntity) == selEntities.end()){
            clearLinePointSelection();
        }
    }
    // a capsule handle the same: its GameObject selected, its capsule there
    if (selectedCapsuleHandleIndex >= 0){
        const CapsuleShadowCasterComponent* caster = scene->findComponent<CapsuleShadowCasterComponent>(selectedCapsuleHandleEntity);
        if (!caster || selectedCapsuleHandleIndex >= (int)caster->capsules.size() * CAPSULE_HANDLES ||
                std::find(selEntities.begin(), selEntities.end(), selectedCapsuleHandleEntity) == selEntities.end()){
            clearCapsuleHandleSelection();
        }
    }

    displaySettings = settings;

    // The tools work on this scene's selection, or on the edit layer's while one is set.
    // selEntities stays this scene's: the overlays of subclasses highlight from it.
    Scene* const target = gizmoScene();
    ToolsLayer& tools = gizmoTools();
    std::vector<Entity> toolEntities = editLayer.active ? editLayer.selection : selEntities;
    const std::vector<Entity>& authoredEntities = editLayer.active ? editLayer.entities : entities;
    if (editLayer.active){
        // the toolbar sets the tool on this scene's tools layer
        tools.setGizmoSelected(toolslayer.getGizmoSelected());
    }

    // a posed model (pose animation) and its bones take no transform gizmo: the pose handles move them
    if (poseViewport && !editLayer.active){
        poseViewport->removeCovered(toolEntities);
    }

    // Physics (G) shows in orange gizmos; it never works through an edit layer's tools
    PhysicsPlacementWindow* physics = Backend::getApp().getPhysicsPlacementWindow();
    const uint32_t physicsSceneId = physics ? physics->findSceneId(scene) : NULL_PROJECT_SCENE;
    toolslayer.setPhysicsTint(physics && physics->isShownInScene(physicsSceneId));

    // while playing, what the running scene created is read-only: no gizmo moves it
    if (editLayer.active ? editLayer.running : isPlaying){
        toolEntities.erase(std::remove_if(toolEntities.begin(), toolEntities.end(), [&authoredEntities](Entity entity){
            return std::find(authoredEntities.begin(), authoredEntities.end(), entity) == authoredEntities.end();
        }), toolEntities.end());
    }

    // Editor-only viewport debug override, for the Scene view only. In edit mode the
    // Engine's frame is the Scene view, so its camera passes cull no face; while playing
    // the frame is the game, which keeps its culling, and the Scene view asks for it as a
    // view of its own (drawSceneView). Free to toggle: nothing reloads.
    scene->getSystem<RenderSystem>()->setDisableFaceCulling(displaySettings.disableFaceCulling && !isPlaying);
    // the Draw Mode the same way (spec S54 R11)
    scene->getSystem<RenderSystem>()->setDrawMode(isPlaying ? ViewDrawMode::SHADED : displaySettings.drawMode);
    scene->getSystem<RenderSystem>()->setDisableFog(displaySettings.disableFog && !isPlaying);
    scene->getSystem<RenderSystem>()->setDisableClouds(displaySettings.disableClouds && !isPlaying);
    // the Game view's Preview Atmosphere Quality is set on the frame while playing (App)
    if (!isPlaying)
        scene->getSystem<RenderSystem>()->setPreviewAtmosphereQuality(-1);

    // Fixed game resolution only applies while playing: edit-mode viewports stay
    // at native resolution (gizmos and overlays would otherwise mismatch).
    scene->getSystem<RenderSystem>()->setDisableFixedResolution(!isPlaying);
    // The scene's post-processing (phase 4) is the game's: off in the edit-mode Scene view unless
    // its display setting shows it or a scene camera is previewed; the Game view's pass (this
    // frame drawn through the game camera) has it (free to toggle: the colour passes bypass the
    // post colour, the uber pass is skipped)
    viewDisablesPostProcessing = !isPlaying && !displaySettings.showPostProcessing && !isPreviewCameraActive();
    scene->getSystem<RenderSystem>()->setDisablePostProcessing(viewDisablesPostProcessing && gameViewCameraEntity == NULL_ENTITY);

    if (!isPlaying){
        syncSceneCamera();
    }
    if (isPreviewCameraActive()){
        hideAllGizmos();
        return;
    }

    Entity cameraEntity = camera->getEntity();
    CameraComponent& cameracomp = scene->getComponent<CameraComponent>(cameraEntity);
    Transform& cameratransform = scene->getComponent<Transform>(cameraEntity);

    Vector3 gizmoPosition;
    Quaternion gizmoRotation;

    bool sameRotation = true;
    Quaternion firstRotation;
    bool firstEntity = true;

    size_t numTEntities = 0;
    std::vector<OBB> selBB;
    OBB totalSelBB;

    multipleEntitiesSelected = toolEntities.size() > 1;

    // the selected points of the lattice the tools edit, which the gizmo then acts on
    const Entity editedLattice = getPointEditLattice(toolEntities);
    const uint32_t latticeScene = getLatticeSceneId();
    const bool latticePointsSelected = editedLattice != NULL_ENTITY && latticeScene != NULL_PROJECT_SCENE &&
        !LatticeEditorOps::getSelectedPoints(latticeScene, editedLattice).empty();
    // the selected knots of the spline in Edit Spline, which the gizmo then acts on
    const Entity editedSpline = getKnotEditSpline(toolEntities);
    const bool splineKnotsSelected = editedSpline != NULL_ENTITY && !SplineEditor::get().getSelection().empty();

    bool showAnchorGizmo = false;
    Vector4 anchorData = Vector4::ZERO; // x: left, y: top, z: right, w: bottom
    Rect anchorArea = Rect(0.0f, 0.0f, 0.0f, 0.0f);

    for (Entity& entity: toolEntities){
        if (Transform* transform = target->findComponent<Transform>(entity)){
            numTEntities++;

            if (toolEntities.size() == 1){
                Entity entity = toolEntities[0];
                if (RectTransformComponent* layout = target->findComponent<RectTransformComponent>(entity)){
                    if (layout->usingAnchors){
                        showAnchorGizmo = true;
                    }
                    anchorData.x = layout->anchorPointLeft;
                    anchorData.y = layout->anchorPointTop;
                    anchorData.z = layout->anchorPointRight;
                    anchorData.w = layout->anchorPointBottom;

                    anchorArea = target->getSystem<UISystem>()->getAnchorReferenceRect(*layout, *transform, true);
                }
            }

            gizmoPosition += transform->worldPosition;

            if (firstEntity) {
                firstRotation = transform->worldRotation;
                firstEntity = false;
            } else if (transform->worldRotation != firstRotation) {
                sameRotation = false;
            }

            if ((!useGlobalTransform && toolEntities.size() == 1) || tools.getGizmoSelected() == GizmoSelected::OBJECT2D){
                gizmoRotation = transform->worldRotation;
            }

            selBB.push_back(getFamilyVisualOBB(target, entity, selectionOffset));

            totalSelBB.enclose(getOBB(target, entity, false));
        }

    }

    tools.setShowAnchorGizmo(showAnchorGizmo);

    auto syncObject2DGizmoMode = [&](){
        if (tools.getGizmoSelected() != GizmoSelected::OBJECT2D){
            return;
        }

        bool showRects = true;
        bool showCross = false;

        if (toolEntities.size() == 1){
            Signature signature = target->getSignature(toolEntities[0]);
            bool isCamera = signature.test(target->getComponentId<CameraComponent>());
            bool isInstancedMesh = signature.test(target->getComponentId<InstancedMeshComponent>());
            bool canResize2D = signature.test(target->getComponentId<RectTransformComponent>())
                || signature.test(target->getComponentId<SpriteComponent>())
                || (signature.test(target->getComponentId<TilemapComponent>()) && selectedTileIndex >= 0);

            showCross = signature.test(target->getComponentId<PointsComponent>()) || signature.test(target->getComponentId<LineRendererComponent>());

            // a selected occluder / polygon / track point is a size-less target: show the move cross
            if (selectedOccluderPointIndex >= 0 && toolEntities[0] == selectedOccluderPointEntity){
                showCross = true;
            }
            if (selectedPolygonPointIndex >= 0 && toolEntities[0] == selectedPolygonPointEntity){
                showCross = true;
            }
            if (selectedJointAnchorIndex >= 0 && toolEntities[0] == selectedJointAnchorEntity){
                showCross = true;
            }
            if (selectedTrackPointIndex >= 0 && toolEntities[0] == selectedTrackPointEntity){
                showCross = true;
            }
            if (latticePointsSelected){
                showCross = true;
            }

            showRects = canResize2D && !isCamera && !isInstancedMesh && !showCross;
        }

        tools.setShowObject2DRects(showRects);
        tools.setShowObject2DCross(showCross);
    };

    syncObject2DGizmoMode();

    // the terrain and Paint Dye brushes take the left button: no gizmo to grab
    bool terrainEditing = isTerrainEditing();
    bool fluidPainting = isFluidPainting();

    totalSelBB.setHalfExtents(totalSelBB.getHalfExtents() + Vector3(selectionOffset));

    // Gizmo screen-size is distance-based in perspective, so it must be computed
    // from the gizmo's actual world position. Sub-selections (instance, tile,
    // point) place the gizmo away from the entity origin, so recompute per
    // position instead of reusing the entity-derived scale (otherwise the gizmo
    // is sized for the entity's distance and looks wrong).
    auto computeGizmoScale = [&](const Vector3& worldPos) -> float {
        return targetBillboardScale(target, worldPos, gizmoScale);
    };

    bool selectionVisibility = false;
    if (numTEntities > 0){
        gizmoPosition /= numTEntities;

        // a physics gesture's handle sits on the objects as they move, wherever the mouse is
        if (physicsGestureStarted && !editLayer.active){
            if (PlacementSession* session = physics->heldGesture(physicsSceneId, PhysicsPlacementWindow::GestureInput::Gizmo)){
                gizmoPosition = session->getMoversCenter() + physicsGizmoOffset;
            }
        }

        selectionVisibility = true;

        float scale = computeGizmoScale(gizmoPosition);

        tools.updateGizmo(camera, gizmoPosition, gizmoRotation, scale, totalSelBB, mouseRay, mouseClicked, anchorData, anchorArea);

        // Override gizmo for selected tile within tilemap
        if (selectedTileIndex >= 0 && toolEntities.size() == 1 && toolEntities[0] == selectedTileEntity
            && tools.getGizmoSelected() == GizmoSelected::OBJECT2D){
            OBB tileOBB = getTileOBB(selectedTileEntity, selectedTileIndex, true);
            if (!tileOBB.isNull()){
                Vector3 tileCenter = tileOBB.getCenter();
                tools.updateGizmo(camera, tileCenter, gizmoRotation, computeGizmoScale(tileCenter), tileOBB, mouseRay, mouseClicked, anchorData, anchorArea);
            }
        }

        // Override gizmo for selected instance within instanced mesh
        if (selectedInstanceIndex >= 0 && toolEntities.size() == 1 && toolEntities[0] == selectedInstanceEntity){
            InstancedMeshComponent* instmesh = target->findComponent<InstancedMeshComponent>(selectedInstanceEntity);
            Transform* instTransform = target->findComponent<Transform>(selectedInstanceEntity);
            if (instmesh && instTransform && selectedInstanceIndex < (int)instmesh->instances.size()){
                OBB instOBB = getInstanceOBB(selectedInstanceEntity, selectedInstanceIndex, true);
                if (!instOBB.isNull()){
                    const InstanceData& inst = instmesh->instances[selectedInstanceIndex];
                    Quaternion instWorldRotation = getInstanceWorldRotation(*instTransform, *instmesh, inst);
                    Vector3 instCenter = instOBB.getCenter();
                    tools.updateGizmo(camera, instCenter, instWorldRotation, computeGizmoScale(instCenter), instOBB, mouseRay, mouseClicked, anchorData, anchorArea);
                    // Replace selection outline with the instance OBB
                    selBB.clear();
                    selBB.push_back(instOBB);
                }
            }
        }

        // Override gizmo for selected occluder polygon point
        if (selectedOccluderPointIndex >= 0 && toolEntities.size() == 1 && toolEntities[0] == selectedOccluderPointEntity
            && tools.getGizmoSelected() == GizmoSelected::OBJECT2D){
            OBB pointOBB = getOccluderPointOBB(selectedOccluderPointEntity, selectedOccluderPointIndex);
            if (!pointOBB.isNull()){
                Vector3 pointCenter = pointOBB.getCenter();
                tools.updateGizmo(camera, pointCenter, gizmoRotation, computeGizmoScale(pointCenter), pointOBB, mouseRay, mouseClicked, anchorData, anchorArea);
            }else{
                // point removed or shape changed: drop the stale sub-selection
                clearOccluderPointSelection();
            }
        }

        // Override gizmo for selected line endpoint (2D gizmo or 3D translate)
        if (selectedLinePointIndex >= 0 && toolEntities.size() == 1 && toolEntities[0] == selectedLinePointEntity
            && (tools.getGizmoSelected() == GizmoSelected::OBJECT2D || tools.getGizmoSelected() == GizmoSelected::TRANSLATE)){
            OBB pointOBB = getLinePointOBB(selectedLinePointEntity, selectedLinePointIndex);
            if (!pointOBB.isNull()){
                Vector3 pointCenter = pointOBB.getCenter();
                tools.updateGizmo(camera, pointCenter, gizmoRotation, computeGizmoScale(pointCenter), pointOBB, mouseRay, mouseClicked, anchorData, anchorArea);
            }else{
                // line removed: drop the stale sub-selection
                clearLinePointSelection();
            }
        }

        // Override gizmo for selected polygon/mesh-polygon vertex (2D gizmo or 3D translate)
        if (selectedPolygonPointIndex >= 0 && toolEntities.size() == 1 && toolEntities[0] == selectedPolygonPointEntity
            && (tools.getGizmoSelected() == GizmoSelected::OBJECT2D || tools.getGizmoSelected() == GizmoSelected::TRANSLATE)){
            OBB pointOBB = getPolygonPointOBB(selectedPolygonPointEntity, selectedPolygonPointIsMesh, selectedPolygonPointIndex);
            if (!pointOBB.isNull()){
                Vector3 pointCenter = pointOBB.getCenter();
                tools.updateGizmo(camera, pointCenter, gizmoRotation, computeGizmoScale(pointCenter), pointOBB, mouseRay, mouseClicked, anchorData, anchorArea);
            }else{
                // vertex removed: drop the stale sub-selection
                clearPolygonPointSelection();
            }
        }

        // Override gizmo for the selected spline knots (W/E/R, or the 2D gizmo's move)
        if (splineKnotsSelected){
            Vector3 pivot;
            Quaternion pivotRotation;
            OBB knotsOBB;
            if (getSplineKnotsPivot(editedSpline, SplineEditor::get().getSelection(), pivot, pivotRotation, knotsOBB)){
                tools.updateGizmo(camera, pivot, pivotRotation, computeGizmoScale(pivot), knotsOBB, mouseRay, mouseClicked, anchorData, anchorArea);
            }
        }

        // Override gizmo for the selected Joint 2D anchor handle
        if (selectedJointAnchorIndex >= 0 && toolEntities.size() == 1 && toolEntities[0] == selectedJointAnchorEntity
            && (tools.getGizmoSelected() == GizmoSelected::OBJECT2D || tools.getGizmoSelected() == GizmoSelected::TRANSLATE)){
            OBB pointOBB = getJointAnchorOBB(selectedJointAnchorEntity, selectedJointAnchorIndex);
            if (!pointOBB.isNull()){
                Vector3 pointCenter = pointOBB.getCenter();
                tools.updateGizmo(camera, pointCenter, gizmoRotation, computeGizmoScale(pointCenter), pointOBB, mouseRay, mouseClicked, anchorData, anchorArea);
            }else{
                clearJointAnchorSelection();
            }
        }

        // Override gizmo for the selected Capsule Shadow Caster handle (3D translate)
        if (selectedCapsuleHandleIndex >= 0 && toolEntities.size() == 1 && toolEntities[0] == selectedCapsuleHandleEntity
            && tools.getGizmoSelected() == GizmoSelected::TRANSLATE && !editLayer.active){
            OBB pointOBB = getCapsuleHandleOBB(selectedCapsuleHandleEntity, selectedCapsuleHandleIndex);
            if (!pointOBB.isNull()){
                Vector3 pointCenter = pointOBB.getCenter();
                tools.updateGizmo(camera, pointCenter, gizmoRotation, computeGizmoScale(pointCenter), pointOBB, mouseRay, mouseClicked, anchorData, anchorArea);
            }else{
                // its bone renamed or gone: drop the stale sub-selection
                clearCapsuleHandleSelection();
            }
        }

        // Override gizmo for the selected lattice points (W/E/R, or the 2D gizmo's move)
        if (latticePointsSelected){
            Vector3 pivot;
            Quaternion pivotRotation;
            OBB pointsOBB;
            if (getLatticePointsPivot(editedLattice, LatticeEditorOps::getSelectedPoints(latticeScene, editedLattice), pivot, pivotRotation, pointsOBB)){
                tools.updateGizmo(camera, pivot, pivotRotation, computeGizmoScale(pivot), pointsOBB, mouseRay, mouseClicked, anchorData, anchorArea);
            }
        }

        if (selBB.size() > 0) {
            if (editLayer.active){
                fillSelectionLines(layerSelLines[layerToolsIndex()], selBB, !editLayer.view.isOrthographic());
            }else{
                updateSelLines(selBB);
            }
        }
    }

    // Gizmo for a selected TranslateTracks keyframe point. The tracks entity has
    // no Transform (numTEntities is 0 for it), so this runs as its own branch.
    if (selectedTrackPointIndex >= 0 && toolEntities.size() == 1 && toolEntities[0] == selectedTrackPointEntity
        && (tools.getGizmoSelected() == GizmoSelected::TRANSLATE || tools.getGizmoSelected() == GizmoSelected::OBJECT2D)){
        OBB pointOBB = getTrackPointOBB(selectedTrackPointEntity, selectedTrackPointIndex);
        if (!pointOBB.isNull()){
            Vector3 pointCenter = pointOBB.getCenter();

            tools.updateGizmo(camera, pointCenter, gizmoRotation, computeGizmoScale(pointCenter), pointOBB, mouseRay, mouseClicked, anchorData, anchorArea);
            selectionVisibility = true;
        }else{
            // point removed or values changed: drop the stale sub-selection
            clearTrackPointSelection();
        }
    }

    toolslayer.updateCamera(cameracomp, cameratransform);
    if (editLayer.active){
        tools.updateCamera(editLayer.view);
    }

    // Determine selLines visibility: hide for single entity with OBJECT2D gizmo or empty selBB
    bool showSelLines = selectionVisibility;
    if (!multipleEntitiesSelected && (tools.getGizmoSelected() == GizmoSelected::OBJECT2D || selBB.size() == 0)) {
        if (selectedTileIndex < 0 && selectedInstanceIndex < 0) {
            showSelLines = false;
        }
    }
    LineRenderer* outline = editLayer.active ? layerSelLines[layerToolsIndex()] : selLines;
    outline->setVisible(showSelLines && !displaySettings.hideSelectionOutline);
    if (editLayer.active){
        // this scene's own gizmo and outline give way to the layer's
        selLines->setVisible(false);
        toolslayer.setGizmoVisible(false);
    }
    for (int i = 0; i < 2; i++){
        if (layerTools[i] && (!editLayer.active || i != layerToolsIndex())){
            layerTools[i]->setGizmoVisible(false);
            layerSelLines[i]->setVisible(false);
        }
    }

    if (terrainEditing || fluidPainting){
        tools.setGizmoVisible(false);
    }else if (tools.getGizmoSelected() == GizmoSelected::OBJECT2D && !sameRotation){
        tools.setGizmoVisible(false);
    }else{
        tools.setGizmoVisible(selectionVisibility);
    }

    if (!terrainEditing && terrainBrushLines){
        terrainBrushLines->setVisible(false);
    }

    syncObject2DGizmoMode();

    updateLatticeCages(selEntities, entities, editedLattice);
    updateSplineOverlay(selEntities, editedSpline);
    updateWireframeOverlay();
    updatePhysicsOverlay(physics, physicsSceneId);
    updateNavigationOverlay(selEntities);

    uilayer.setViewGizmoImageVisible(true);
}

void editor::SceneRender::mouseHoverEvent(float x, float y){
    ownerMouseRay = screenToRay(x, y);
    mouseRay = editLayer.active ? gizmoRay(x, y) : ownerMouseRay;
    updateTerrainBrushCursor();
    if (poseViewport){
        poseViewport->hover(ownerMouseRay);
    }
}

bool editor::SceneRender::isPoseEditing() const{
    return poseViewport && !isPlaying && poseViewport->isPosing();
}

bool editor::SceneRender::poseMouseDown(float x, float y, bool shift, bool ctrl){
    if (!isPoseEditing()){
        return false;
    }
    ownerMouseRay = screenToRay(x, y);
    return poseViewport->mouseDown(ownerMouseRay, shift, ctrl);
}

bool editor::SceneRender::isPoseDragging() const{
    return poseViewport && poseViewport->isDragging();
}

void editor::SceneRender::mouseClickEvent(float x, float y, std::vector<Entity> selEntities){
    mouseClicked = true;

    if (poseViewport && !editLayer.active){
        poseViewport->removeCovered(selEntities);
    }

    if (TerrainEditWindow* terrainEditWindow = Backend::getApp().getTerrainEditWindow()){
        if (terrainEditWindow->isEditingScene(scene)){
            terrainEditWindow->beginStroke(scene, ownerMouseRay);
            updateTerrainBrushCursor();
            return;
        }
    }

    // a selected NavMesh Link's start or end handle under the press: its drag (S19 R34)
    if (NavigationEditor::get().beginLinkHandleDrag(Backend::getApp().getProject(), getLatticeSceneId(), scene, ownerMouseRay, selEntities)){
        physicsClick = false;
        return;
    }

    // the nav-grid brush (S19 R35): a press on the selected NavGrid starts a stroke
    if (NavigationEditor::get().isBrushPainting(scene)){
        physicsClick = false;
        NavigationEditor::get().beginBrushStroke(Backend::getApp().getProject(), scene, ownerMouseRay, ImGui::GetIO().KeyShift);
        return;
    }

    // Paint Dye: a press on the fluid's Sprite starts a stroke; elsewhere it does nothing
    // (the brush holds the left button until Esc or its toggle)
    if (FluidEditorTools::isPainting(scene)){
        physicsClick = false;
        FluidEditorTools::beginStroke(scene, ownerMouseRay);
        return;
    }

    // a drag from this click is physics' when Physics works here (sub-selected points,
    // instances, tiles and lattice points keep their own drags)
    physicsDragBegun = false;
    physicsGestureStarted = false;
    {
        PhysicsPlacementWindow* physics = Backend::getApp().getPhysicsPlacementWindow();
        physicsClick = physics && !editLayer.active && physics->isActiveInScene(physics->findSceneId(scene)) && !hasToolSubSelection(selEntities);
    }

    // the tools work on the edit layer's selection while one is set
    Scene* const target = gizmoScene();
    ToolsLayer& tools = gizmoTools();
    if (editLayer.active){
        selEntities = editLayer.selection;
    }
    // a driven Lattice Follower: the gizmo moves its rest (LatticeSceneRender.cpp)
    redirectDrivenFollowers(target, selEntities);

    Vector3 viewDir = gizmoViewDirection();

    Vector3 gizmoPosition = tools.getGizmoPosition();
    Quaternion gizmoRotation = tools.getGizmoRotation();
    Matrix4 gizmoRMatrix = gizmoRotation.getRotationMatrix();
    Matrix4 gizmoMatrix = Matrix4::translateMatrix(gizmoPosition) * gizmoRMatrix * Matrix4::scaleMatrix(Vector3(1,1,1));

    float dotX = viewDir.dotProduct(gizmoRMatrix * Vector3(1,0,0));
    float dotY = viewDir.dotProduct(gizmoRMatrix * Vector3(0,1,0));
    float dotZ = viewDir.dotProduct(gizmoRMatrix * Vector3(0,0,1));

    if (tools.getGizmoSelected() == GizmoSelected::TRANSLATE || tools.getGizmoSelected() == GizmoSelected::SCALE){
        if (tools.getGizmoSideSelected() == GizmoSideSelected::XYZ){
            cursorPlane = Plane((gizmoRMatrix * Vector3(dotX, dotY, dotZ).normalize()), gizmoPosition);
        }else if (tools.getGizmoSideSelected() == GizmoSideSelected::X){
            cursorPlane = Plane((gizmoRMatrix * Vector3(0, dotY, dotZ).normalize()), gizmoPosition);
        }else if (tools.getGizmoSideSelected() == GizmoSideSelected::Y){
            cursorPlane = Plane((gizmoRMatrix * Vector3(dotX, 0, dotZ).normalize()), gizmoPosition);
        }else if (tools.getGizmoSideSelected() == GizmoSideSelected::Z){
            cursorPlane = Plane((gizmoRMatrix * Vector3(dotX, dotY, 0).normalize()), gizmoPosition);
        }else if (tools.getGizmoSideSelected() == GizmoSideSelected::XY){
            cursorPlane = Plane((gizmoRMatrix * Vector3(0, 0, dotZ).normalize()), gizmoPosition);
        }else if (tools.getGizmoSideSelected() == GizmoSideSelected::XZ){
            cursorPlane = Plane((gizmoRMatrix * Vector3(0, dotY, 0).normalize()), gizmoPosition);
        }else if (tools.getGizmoSideSelected() == GizmoSideSelected::YZ){
            cursorPlane = Plane((gizmoRMatrix * Vector3(dotX, 0, 0).normalize()), gizmoPosition);
        }
    }

    if (tools.getGizmoSelected() == GizmoSelected::ROTATE){
        cursorPlane = Plane((gizmoRMatrix * Vector3(dotX, dotY, dotZ).normalize()), gizmoPosition);

        if (tools.getGizmoSideSelected() == GizmoSideSelected::X){
            rotationAxis = gizmoRMatrix * Vector3(dotX, 0.0, 0.0).normalize();
        }else if(tools.getGizmoSideSelected() == GizmoSideSelected::Y){
            rotationAxis = gizmoRMatrix * Vector3(0.0, dotY, 0.0).normalize();
        }else if(tools.getGizmoSideSelected() == GizmoSideSelected::Z){
            rotationAxis = gizmoRMatrix * Vector3(0.0, 0.0, dotZ).normalize();
        }else{
            rotationAxis = cursorPlane.normal;
        }
    }

    if (tools.getGizmoSelected() == GizmoSelected::OBJECT2D){
        cursorPlane = Plane(Vector3(0, 0, 1), gizmoPosition);
    }

    RayReturn rretrun = mouseRay.intersects(cursorPlane);
    objectMatrixOffset.clear();
    anchoredMoveStart.clear();
    if (rretrun){
        gizmoStartPosition = gizmoPosition;
        cursorStartOffset = gizmoPosition - rretrun.point;
        rotationStartOffset = gizmoRotation;

        for (Entity& entity: selEntities){
            Signature signature = target->getSignature(entity);
            if (signature.test(target->getComponentId<Transform>())){
                Transform& transform = target->getComponent<Transform>(entity);
                objectMatrixOffset[entity] = gizmoMatrix.inverse() * transform.modelMatrix;
            }
            if (signature.test(target->getComponentId<SpriteComponent>())){
                SpriteComponent& sprite = target->getComponent<SpriteComponent>(entity);
                objectSizeOffset[entity] = Vector2(sprite.width, sprite.height);
            }
            if (signature.test(target->getComponentId<TilemapComponent>())){
                TilemapComponent& tilemap = target->getComponent<TilemapComponent>(entity);
                objectSizeOffset[entity] = Vector2(tilemap.width, tilemap.height);
            }
            if (signature.test(target->getComponentId<RectTransformComponent>())){
                RectTransformComponent& layout = target->getComponent<RectTransformComponent>(entity);
                objectSizeOffset[entity] = Vector2(layout.width, layout.height);
                if (layout.usingAnchors && layout.anchorPreset != AnchorPreset::NONE && signature.test(target->getComponentId<Transform>())){
                    anchoredMoveStart[entity] = {layout.positionOffset, target->getComponent<Transform>(entity).position};
                }
            }
        }

        // Store tile start state for tile sub-selection drag
        if (selectedTileIndex >= 0 && selEntities.size() == 1 && selEntities[0] == selectedTileEntity){
            TilemapComponent* tilemap = target->findComponent<TilemapComponent>(selectedTileEntity);
            if (tilemap && selectedTileIndex < (int)tilemap->numTiles){
                tileStartPosition = tilemap->tiles[selectedTileIndex].position;
                tileStartWidth = tilemap->tiles[selectedTileIndex].width;
                tileStartHeight = tilemap->tiles[selectedTileIndex].height;

                // Override gizmo start position to tile center in world space
                OBB tileOBB = getTileOBB(selectedTileEntity, selectedTileIndex);
                if (!tileOBB.isNull()){
                    gizmoStartPosition = tileOBB.getCenter();
                    cursorStartOffset = gizmoStartPosition - rretrun.point;
                }
            }
        }

        // Override gizmo start position to the selected occluder point (the drag
        // follows the cursor absolutely, so no start value is needed)
        if (selectedOccluderPointIndex >= 0 && selEntities.size() == 1 && selEntities[0] == selectedOccluderPointEntity){
            OBB pointOBB = getOccluderPointOBB(selectedOccluderPointEntity, selectedOccluderPointIndex);
            if (!pointOBB.isNull()){
                gizmoStartPosition = pointOBB.getCenter();
                cursorStartOffset = gizmoStartPosition - rretrun.point;
            }
        }

        // Override gizmo start position to the selected line endpoint
        if (selectedLinePointIndex >= 0 && selEntities.size() == 1 && selEntities[0] == selectedLinePointEntity){
            OBB pointOBB = getLinePointOBB(selectedLinePointEntity, selectedLinePointIndex);
            if (!pointOBB.isNull()){
                gizmoStartPosition = pointOBB.getCenter();
                cursorStartOffset = gizmoStartPosition - rretrun.point;
            }
        }

        // Override gizmo start position to the selected polygon vertex
        if (selectedPolygonPointIndex >= 0 && selEntities.size() == 1 && selEntities[0] == selectedPolygonPointEntity){
            OBB pointOBB = getPolygonPointOBB(selectedPolygonPointEntity, selectedPolygonPointIsMesh, selectedPolygonPointIndex);
            if (!pointOBB.isNull()){
                gizmoStartPosition = pointOBB.getCenter();
                cursorStartOffset = gizmoStartPosition - rretrun.point;
            }
        }

        // Override gizmo start position to the selected Joint 2D anchor
        if (selectedJointAnchorIndex >= 0 && selEntities.size() == 1 && selEntities[0] == selectedJointAnchorEntity){
            OBB pointOBB = getJointAnchorOBB(selectedJointAnchorEntity, selectedJointAnchorIndex);
            if (!pointOBB.isNull()){
                gizmoStartPosition = pointOBB.getCenter();
                cursorStartOffset = gizmoStartPosition - rretrun.point;
            }
        }

        // Override gizmo start position to the selected Capsule Shadow Caster handle
        if (selectedCapsuleHandleIndex >= 0 && selEntities.size() == 1 && selEntities[0] == selectedCapsuleHandleEntity){
            OBB pointOBB = getCapsuleHandleOBB(selectedCapsuleHandleEntity, selectedCapsuleHandleIndex);
            if (!pointOBB.isNull()){
                gizmoStartPosition = pointOBB.getCenter();
                cursorStartOffset = gizmoStartPosition - rretrun.point;
            }
            const CapsuleShadowCasterComponent* caster = scene->findComponent<CapsuleShadowCasterComponent>(selectedCapsuleHandleEntity);
            capsuleDragBase = caster ? caster->capsules : std::vector<CapsuleShadowShape>();
        }

        // Override gizmo start position to the selected track keyframe point
        if (selectedTrackPointIndex >= 0 && selEntities.size() == 1 && selEntities[0] == selectedTrackPointEntity){
            OBB pointOBB = getTrackPointOBB(selectedTrackPointEntity, selectedTrackPointIndex);
            if (!pointOBB.isNull()){
                gizmoStartPosition = pointOBB.getCenter();
                cursorStartOffset = gizmoStartPosition - rretrun.point;
            }
        }

        // Store instance start state for instance sub-selection drag
        if (selectedInstanceIndex >= 0 && selEntities.size() == 1 && selEntities[0] == selectedInstanceEntity){
            InstancedMeshComponent* instmesh = target->findComponent<InstancedMeshComponent>(selectedInstanceEntity);
            Transform* instTransform = target->findComponent<Transform>(selectedInstanceEntity);
            if (instmesh && instTransform && selectedInstanceIndex < (int)instmesh->instances.size()){
                const InstanceData& inst = instmesh->instances[selectedInstanceIndex];

                OBB instOBB = getInstanceOBB(selectedInstanceEntity, selectedInstanceIndex);
                if (!instOBB.isNull()){
                    // Gizmo is centered on the instance OBB center (world space); its
                    // orientation must match getInstanceOBB (see getInstanceWorldRotation).
                    gizmoStartPosition = instOBB.getCenter();
                    cursorStartOffset = gizmoStartPosition - rretrun.point;
                    rotationStartOffset = getInstanceWorldRotation(*instTransform, *instmesh, inst);

                    // Build the actual instance world matrix (same formula the renderer uses
                    // in non-billboard mode) so drag math operates on the instance directly.
                    Matrix4 instanceLocalMatrix = Matrix4::translateMatrix(inst.position)
                                                  * inst.rotation.getRotationMatrix()
                                                  * Matrix4::scaleMatrix(inst.scale);
                    Matrix4 instanceWorldMatrix = instTransform->modelMatrix * instanceLocalMatrix;

                    Matrix4 gizmoM = Matrix4::translateMatrix(gizmoStartPosition)
                                     * rotationStartOffset.getRotationMatrix()
                                     * Matrix4::scaleMatrix(Vector3(1, 1, 1));
                    objectMatrixOffset[selectedInstanceEntity] = gizmoM.inverse() * instanceWorldMatrix;
                }
            }
        }

        // a gizmo handle grabbed on selected lattice points: their drag begins
        latticeDrag = LatticeDrag();
        Entity latticeEntity = getPointEditLattice(selEntities);
        const bool handleGrabbed = tools.getGizmoSelected() == GizmoSelected::OBJECT2D
            ? tools.getGizmo2DSideSelected() == Gizmo2DSideSelected::CENTER
            : tools.getGizmoSideSelected() != GizmoSideSelected::NONE;
        if (latticeEntity != NULL_ENTITY && handleGrabbed && getLatticeSceneId() != NULL_PROJECT_SCENE &&
            !LatticeEditorOps::getSelectedPoints(getLatticeSceneId(), latticeEntity).empty()){
            beginLatticeDrag(latticeEntity, gizmoPosition, gizmoRotation);
        }
        // and on selected spline knots
        splineDrag = SplineDrag();
        const Entity splineEntity = getKnotEditSpline(selEntities);
        if (splineEntity != NULL_ENTITY && handleGrabbed && !SplineEditor::get().getSelection().empty()){
            beginSplineDrag(splineEntity, gizmoPosition, gizmoRotation);
        }
    }

}

void editor::SceneRender::mouseReleaseEvent(float x, float y){
    uilayer.setSelectionBoxVisible(false);

    // a drag's edit-mode trail preview ends (its points cleared)
    if (std::shared_ptr<TrailSystem> trails = scene->findSystem<TrailSystem>()){
        trails->clearPreviews();
    }

    if (poseViewport){
        // a pose handle drag ends: one undo step
        poseViewport->mouseUp();
    }

    if (TerrainEditWindow* terrainEditWindow = Backend::getApp().getTerrainEditWindow()){
        terrainEditWindow->endStroke();
    }
    FluidEditorTools::endStroke();
    NavigationEditor::get().endBrushStroke(Backend::getApp().getProject());
    NavigationEditor::get().endLinkHandleDrag(Backend::getApp().getProject());

    // a physics drag ends its gesture (commit, or settle then commit)
    if (physicsGestureStarted){
        if (PhysicsPlacementWindow* physics = Backend::getApp().getPhysicsPlacementWindow()){
            physics->endGesture(physics->findSceneId(scene), PhysicsPlacementWindow::GestureInput::Gizmo);
        }
    }
    physicsClick = false;
    physicsDragBegun = false;
    physicsGestureStarted = false;

    mouseClicked = false;

    toolslayer.mouseRelease();
    for (auto& tools : layerTools){
        if (tools){
            tools->mouseRelease();
        }
    }

    // a recorded drag: one key edit (one merge key) per drag
    endGizmoRecord();

    // one drag of lattice points (or spline knots), one undo step
    endLatticeDrag();
    endSplineDrag();
    dragEdit.commit();
    dragEditStarted = false;
}

bool editor::SceneRender::pushDragCommand(size_t sceneId, Command* command){
    if (dragEditStarted && !dragEdit.isOpen()){
        // Esc cancelled the drag (or a popup ended it): nothing more until the mouse is released
        delete command;
        return false;
    }
    CommandHistory* history = CommandHandle::get(sceneId);
    if (!dragEditStarted){
        dragEdit = history->beginInteractive(std::string());
        dragEditStarted = true;
    }
    return history->addCommand(command);
}

namespace {
    bool gizmoChannel(editor::GizmoSelected gizmo, ClipChannel& channel){
        if (gizmo == editor::GizmoSelected::TRANSLATE) channel = ClipChannel::Position;
        else if (gizmo == editor::GizmoSelected::ROTATE) channel = ClipChannel::Rotation;
        else if (gizmo == editor::GizmoSelected::SCALE) channel = ClipChannel::Scale;
        else return false;
        return true;
    }
}

bool editor::SceneRender::gizmoRecordClaims(uint32_t sceneId, GizmoSelected gizmo, const std::vector<Entity>& selection) const{
    ClipChannel channel;
    return gizmoChannel(gizmo, channel) && ClipRecordHooks::claimsAny(sceneId, selection, channel);
}

bool editor::SceneRender::recordGizmoTransform(uint32_t sceneId, Entity entity, const Matrix4& localMatrix, GizmoSelected gizmo,
                                               bool selectionClaimed){
    ClipChannel channel;
    if (!selectionClaimed || !gizmoChannel(gizmo, channel)) return false;
    // an unbound GameObject of the selection stays as it is (no scene step mixed into the key edit)
    if (!ClipRecordHooks::claims(sceneId, entity, channel)) return true;
    Vector3 position, scale;
    Quaternion rotation;
    localMatrix.decomposeStandard(position, scale, rotation);
    // one merge key per drag, the gizmo's own (another source's gesture is never joined)
    const std::string mergeKey = ClipRecordHooks::gestureMergeKey(RecordSource::Gizmo, nullptr);
    ClipRecordHooks::record(sceneId, entity, ClipRecordHooks::transformValue(channel, position, rotation, scale), mergeKey);
    // Rotate / Scale about a pivot that is not the GameObject's origin (a selection's Center)
    // moves it too: the position is keyed with the drag, as without Record (T3c review M2)
    if (channel != ClipChannel::Position && ClipRecordHooks::claims(sceneId, entity, ClipChannel::Position)){
        Scene* target = gizmoScene();
        const Transform* transform = target ? target->findComponent<Transform>(entity) : nullptr;
        if (transform && (transform->position - position).length() > 1e-5f){
            ClipRecordHooks::record(sceneId, entity, ClipRecordHooks::transformValue(ClipChannel::Position, position, rotation, scale), mergeKey);
        }
    }
    return true;
}

editor::Command* editor::SceneRender::buildObjectTransformDrag(Project* project, uint32_t sceneId, Entity entity, const Matrix4& localMatrix,
                                                       GizmoSelected gizmo, bool selectionClaimed){
    if (recordGizmoTransform(sceneId, entity, localMatrix, gizmo, selectionClaimed)) return nullptr;
    return new ObjectTransformCmd(project, sceneId, entity, localMatrix);
}

void editor::SceneRender::endGizmoRecord(){
    gizmoRecordNoted = false;
    ClipRecordHooks::endGesture(RecordSource::Gizmo);
}

Vector3 editor::SceneRender::getMatrixScale(const Matrix4& matrix){
    return Vector3(
        Vector3(matrix[0][0], matrix[0][1], matrix[0][2]).length(),
        Vector3(matrix[1][0], matrix[1][1], matrix[1][2]).length(),
        Vector3(matrix[2][0], matrix[2][1], matrix[2][2]).length());
}

bool editor::SceneRender::isCorner2DGizmoSide(Gizmo2DSideSelected side){
    return side == Gizmo2DSideSelected::NX_NY || side == Gizmo2DSideSelected::NX_PY ||
           side == Gizmo2DSideSelected::PX_NY || side == Gizmo2DSideSelected::PX_PY;
}

bool editor::SceneRender::gizmo2DSideUsesNegativeX(Gizmo2DSideSelected side){
    return side == Gizmo2DSideSelected::NX_NY || side == Gizmo2DSideSelected::NX_PY;
}

bool editor::SceneRender::gizmo2DSideUsesNegativeY(Gizmo2DSideSelected side){
    return side == Gizmo2DSideSelected::NX_NY || side == Gizmo2DSideSelected::PX_NY;
}

Vector2 editor::SceneRender::lockObject2DAspectRatio(const Vector2& startSize, const Vector2& candidateSize){
    if (startSize.x <= 0.0f || startSize.y <= 0.0f){
        return candidateSize;
    }

    float scaleX = candidateSize.x / startSize.x;
    float scaleY = candidateSize.y / startSize.y;
    float chosenScale = (std::fabs(scaleX - 1.0f) >= std::fabs(scaleY - 1.0f)) ? scaleX : scaleY;

    if (!std::isfinite(chosenScale)){
        return candidateSize;
    }

    if (chosenScale < 0.0f){
        chosenScale = 0.0f;
    }

    return Vector2(startSize.x * chosenScale, startSize.y * chosenScale);
}

void editor::SceneRender::mouseDragEvent(float x, float y, float origX, float origY, Project* project, size_t sceneId, std::vector<Entity> selEntities, bool disableSelection, bool invertRotationSnap, bool preserveAspectRatio){
    // S41 R21 Pixel Move Snap: a 2D scene drawn through a Pixel Perfect Camera moves and sizes
    // on whole art pixels; Ctrl (the snap modifier) drags freely, on-grid being the norm there
    const bool pixelMoveSnap = displaySettings.pixelMoveSnap && !invertRotationSnap && dynamic_cast<SceneRender2D*>(this) &&
                               pixelart::scenePixelPerfect(project ? project->getScene((uint32_t)sceneId) : nullptr);
    ownerMouseRay = screenToRay(x, y);
    mouseRay = editLayer.active ? gizmoRay(x, y) : ownerMouseRay;

    if (poseViewport && poseViewport->isDragging()){
        // a pose handle: Shift keeps it on the horizontal plane, Ctrl/Cmd snaps ring turns
        poseViewport->mouseDrag(ownerMouseRay, preserveAspectRatio, invertRotationSnap);
        return;
    }
    if (poseViewport && !editLayer.active){
        poseViewport->removeCovered(selEntities);
    }

    if (TerrainEditWindow* terrainEditWindow = Backend::getApp().getTerrainEditWindow()){
        if (terrainEditWindow->isEditingScene(scene)){
            terrainEditWindow->paintStroke(scene, ownerMouseRay);
            updateTerrainBrushCursor();
            return;
        }
    }

    if (FluidEditorTools::isPainting(scene)){
        FluidEditorTools::paintStroke(scene, ownerMouseRay);
        return;
    }

    if (NavigationEditor::get().isBrushPainting(scene)){
        NavigationEditor::get().paintBrushStroke(scene, ownerMouseRay, ImGui::GetIO().KeyShift);
        return;
    }

    if (NavigationEditor::get().isDraggingLinkHandle()){
        NavigationEditor::get().dragLinkHandle(scene, ownerMouseRay);
        return;
    }

    // edit mode: the Trail Renderers on and under the GameObjects the gizmo drags draw their
    // trail while dragging (TrailSystem's preview, cleared on release)
    if (isAnyGizmoSideSelected()){
        std::shared_ptr<TrailSystem> trails = scene->findSystem<TrailSystem>();
        if (trails && trails->isPaused() && !selEntities.empty()){
            auto trailArray = scene->getComponentArray<TrailRendererComponent>();
            for (size_t i = 0; i < trailArray->size(); i++){
                const Entity trailEntity = trailArray->getEntity(i);
                for (Entity ancestor = trailEntity; ancestor != NULL_ENTITY; ){
                    if (std::find(selEntities.begin(), selEntities.end(), ancestor) != selEntities.end()){
                        trails->setPreview(trailEntity, true);
                        break;
                    }
                    const Transform* ancestorTransform = scene->findComponent<Transform>(ancestor);
                    ancestor = ancestorTransform ? ancestorTransform->parent : NULL_ENTITY;
                }
            }
        }
    }

    // the box selects while playing too, as on release (Project::selectObjectsByRect)
    if (!disableSelection){
        uilayer.setSelectionBoxVisible(true);
        uilayer.updateRect(Vector2(origX, origY), Vector2(x, y) - Vector2(origX, origY));
    }

    // Physics on: the handle drives the simulation, the gizmo applies no transform of its own
    if (physicsClick){
        if (PhysicsPlacementWindow* physics = Backend::getApp().getPhysicsPlacementWindow()){
            physicsGizmoDrag(physics, (uint32_t)sceneId, invertRotationSnap);
        }
        return;
    }

    // the tools work on the edit layer's selection while one is set, and its commands
    // target the layer; this scene's history keeps them all the same
    Scene* const target = gizmoScene();
    ToolsLayer& tools = gizmoTools();
    const uint32_t commandSceneId = editLayer.active ? editLayer.sceneId : (uint32_t)sceneId;
    if (editLayer.active){
        selEntities = editLayer.selection;
    }
    redirectDrivenFollowers(target, selEntities);

    Vector3 gizmoPosition = tools.getGizmoPosition();
    Matrix4 gizmoRMatrix = tools.getGizmoRotation().getRotationMatrix();

    // Lattice point drag: the gizmo's change goes to the points, not to the lattice entity
    if (latticeDrag.active){
        dragLatticeGizmo(project, commandSceneId, invertRotationSnap);
        return;
    }
    // the same for spline knots
    if (splineDrag.active){
        dragSplineGizmo(project, commandSceneId, invertRotationSnap);
        return;
    }

    // TranslateTracks keyframe point drag: the tracks entity has no Transform,
    // so it is handled outside the per-entity transform loop below.
    if (selectedTrackPointIndex >= 0 && selEntities.size() == 1 && selEntities[0] == selectedTrackPointEntity){
        Entity trackEntity = selectedTrackPointEntity;
        TranslateTracksComponent* tracks = target->findComponent<TranslateTracksComponent>(trackEntity);
        RayReturn rretrun = mouseRay.intersects(cursorPlane);

        if (tracks && rretrun && selectedTrackPointIndex < (int)tracks->values.size()){
            std::string pointProp = "values[" + std::to_string(selectedTrackPointIndex) + "]";

            if (tools.getGizmoSelected() == GizmoSelected::TRANSLATE
                && tools.getGizmoSideSelected() != GizmoSideSelected::NONE){
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

                Vector3 newPos = gizmoStartPosition;
                if (tools.getGizmoSideSelected() == GizmoSideSelected::XYZ){
                    newPos = gizmoStartPosition + (gizmoRMatrix * deltaPos);
                }else if (tools.getGizmoSideSelected() == GizmoSideSelected::X){
                    newPos = gizmoStartPosition + (gizmoRMatrix * Vector3(deltaPos.x, 0, 0));
                }else if (tools.getGizmoSideSelected() == GizmoSideSelected::Y){
                    newPos = gizmoStartPosition + (gizmoRMatrix * Vector3(0, deltaPos.y, 0));
                }else if (tools.getGizmoSideSelected() == GizmoSideSelected::Z){
                    newPos = gizmoStartPosition + (gizmoRMatrix * Vector3(0, 0, deltaPos.z));
                }else if (tools.getGizmoSideSelected() == GizmoSideSelected::XY){
                    newPos = gizmoStartPosition + (gizmoRMatrix * Vector3(deltaPos.x, deltaPos.y, 0));
                }else if (tools.getGizmoSideSelected() == GizmoSideSelected::XZ){
                    newPos = gizmoStartPosition + (gizmoRMatrix * Vector3(deltaPos.x, 0, deltaPos.z));
                }else if (tools.getGizmoSideSelected() == GizmoSideSelected::YZ){
                    newPos = gizmoStartPosition + (gizmoRMatrix * Vector3(0, deltaPos.y, deltaPos.z));
                }

                Vector3 newLocalPos = getTrackPointsWorldMatrix(trackEntity).inverse() * newPos;
                Command* dragCommand = new PropertyCmd<Vector3>(project, commandSceneId, trackEntity,
                    ComponentType::TranslateTracksComponent, pointProp, newLocalPos);
                pushDragCommand(sceneId, dragCommand);

            }else if (tools.getGizmoSelected() == GizmoSelected::OBJECT2D
                && tools.getGizmo2DSideSelected() == Gizmo2DSideSelected::CENTER){
                tools.mouseDrag(rretrun.point);

                // new world position of the point, following the cursor
                Vector3 newWorldPos = rretrun.point + cursorStartOffset;

                if (displaySettings.snapToGrid){
                    float spacing = displaySettings.gridSpacing2D;
                    if (spacing > 0.0f){
                        newWorldPos.x = std::round(newWorldPos.x / spacing) * spacing;
                        newWorldPos.y = std::round(newWorldPos.y / spacing) * spacing;
                    }
                }

                // the cursor plane is at the gizmo's Z: keep the point's own local Z
                Vector3 oldLocal = tracks->values[selectedTrackPointIndex];
                Vector3 newLocalPos = getTrackPointsWorldMatrix(trackEntity).inverse() * newWorldPos;
                newLocalPos.z = oldLocal.z;

                Command* dragCommand = new PropertyCmd<Vector3>(project, commandSceneId, trackEntity,
                    ComponentType::TranslateTracksComponent, pointProp, newLocalPos);
                pushDragCommand(sceneId, dragCommand);
            }
        }
    }

    // Record (animation clips 9.R5): whether it takes this drag of the selection, asked once per
    // drag event (not per selected entity)
    const bool recordSelection = gizmoRecordClaims(commandSceneId, tools.getGizmoSelected(), selEntities);

    for (Entity& entity: selEntities){
        if (selectedTrackPointIndex >= 0 && entity == selectedTrackPointEntity){
            // the drag targets the track keyframe point (handled above), not the entity
            continue;
        }
        Transform* transform = target->findComponent<Transform>(entity);
        if (transform){
            Command* dragCommand = nullptr;
            RayReturn rretrun = mouseRay.intersects(cursorPlane);

    
            bool isInstanceDrag = selectedInstanceIndex >= 0 && selEntities.size() == 1
                                  && entity == selectedInstanceEntity
                                  && target->findComponent<InstancedMeshComponent>(entity) != nullptr;

            auto emitInstanceTransformCmd = [&](const Matrix4& worldMatrix){
                Matrix4 instLocalMatrix = transform->modelMatrix.inverse() * worldMatrix;
                Vector3 newPos, newScale;
                Quaternion newRot;
                instLocalMatrix.decomposeStandard(newPos, newScale, newRot);

                std::string prefix = "instances[" + std::to_string(selectedInstanceIndex) + "]";
                MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                GizmoSelected gz = tools.getGizmoSelected();
                if (gz == GizmoSelected::TRANSLATE || gz == GizmoSelected::OBJECT2D){
                    multiCmd->addPropertyCmd<Vector3>(project, commandSceneId, entity, ComponentType::InstancedMeshComponent, prefix + ".position", newPos);
                }else if (gz == GizmoSelected::ROTATE){
                    multiCmd->addPropertyCmd<Quaternion>(project, commandSceneId, entity, ComponentType::InstancedMeshComponent, prefix + ".rotation", newRot);
                }else if (gz == GizmoSelected::SCALE){
                    multiCmd->addPropertyCmd<Vector3>(project, commandSceneId, entity, ComponentType::InstancedMeshComponent, prefix + ".scale", newScale);
                }
                dragCommand = multiCmd;
            };

            if (rretrun){

                tools.mouseDrag(rretrun.point);

                Transform* transformParent = target->findComponent<Transform>(transform->parent);

                // Emit a transform command for the active drag target (instance or entity).
                // worldMatrix is the new object world matrix built by the current gizmo branch.
                auto applyObjectTransform = [&](const Matrix4& worldMatrix){
                    if (tools.getGizmoSideSelected() == GizmoSideSelected::NONE) return;
                    if (isInstanceDrag){
                        emitInstanceTransformCmd(worldMatrix);
                    }else{
                        Matrix4 m = worldMatrix;
                        if (transformParent){
                            m = transformParent->modelMatrix.inverse() * m;
                        }
                        RectTransformComponent* layout = target->findComponent<RectTransformComponent>(entity);
                        if (layout && anchoredMoveStart.count(entity)){
                            // the move goes to positionOffset; rotation and scale stay the Transform's
                            Vector3 newPosition, newScale;
                            Quaternion newRotation;
                            m.decomposeStandard(newPosition, newScale, newRotation);
                            newPosition -= getUIPivotShift(*layout, newRotation, newScale);

                            MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                            multiCmd->addPropertyCmd<Vector2>(project, commandSceneId, entity, ComponentType::RectTransformComponent,
                                "positionOffset", getAnchoredPositionOffset(target, entity, newPosition, Vector2(layout->width, layout->height)));
                            multiCmd->addCommand(std::make_unique<ObjectTransformCmd>(project, commandSceneId, entity,
                                transform->position, newRotation, newScale));
                            dragCommand = multiCmd;
                        }else{
                            // (Record keys a claimed channel instead: no command, animation clips 9.R5)
                            dragCommand = buildObjectTransformDrag(project, commandSceneId, entity, m, tools.getGizmoSelected(), recordSelection);
                        }
                    }
                };

                if (tools.getGizmoSelected() == GizmoSelected::TRANSLATE){
                    Vector3 deltaPos = gizmoRMatrix.inverse() * ((rretrun.point + cursorStartOffset) - gizmoStartPosition);

                    if (displaySettings.snapToGrid){
                        float spacing = displaySettings.gridSpacing3D;
                        if (spacing > 0.0f){
                            deltaPos.x = std::round(deltaPos.x / spacing) * spacing;
                            deltaPos.y = std::round(deltaPos.y / spacing) * spacing;
                            deltaPos.z = std::round(deltaPos.z / spacing) * spacing;
                        }
                    }

                    Vector3 newPos;
                    // (snapped to whole art pixels below: Pixel Move Snap)
                    if (tools.getGizmoSideSelected() == GizmoSideSelected::XYZ){
                        newPos = gizmoStartPosition + (gizmoRMatrix * deltaPos);
                    }else if (tools.getGizmoSideSelected() == GizmoSideSelected::X){
                        newPos = gizmoStartPosition + (gizmoRMatrix * Vector3(deltaPos.x, 0, 0));
                    }else if(tools.getGizmoSideSelected() == GizmoSideSelected::Y){
                        newPos = gizmoStartPosition + (gizmoRMatrix * Vector3(0, deltaPos.y, 0));
                    }else if(tools.getGizmoSideSelected() == GizmoSideSelected::Z){
                        newPos = gizmoStartPosition + (gizmoRMatrix * Vector3(0, 0, deltaPos.z));
                    }else if (tools.getGizmoSideSelected() == GizmoSideSelected::XY){
                        newPos = gizmoStartPosition + (gizmoRMatrix * Vector3(deltaPos.x, deltaPos.y, 0));
                    }else if (tools.getGizmoSideSelected() == GizmoSideSelected::XZ){
                        newPos = gizmoStartPosition + (gizmoRMatrix * Vector3(deltaPos.x, 0, deltaPos.z));
                    }else if (tools.getGizmoSideSelected() == GizmoSideSelected::YZ){
                        newPos = gizmoStartPosition + (gizmoRMatrix * Vector3(0, deltaPos.y, deltaPos.z));
                    }
                    // S41 R21 Pixel Move Snap: whole art pixels (Ctrl held: free)
                    if (pixelMoveSnap){
                        newPos.x = jasperengine::pixelart::snapWorld(newPos.x, Engine::getPixelsPerUnit());
                        newPos.y = jasperengine::pixelart::snapWorld(newPos.y, Engine::getPixelsPerUnit());
                    }

                    if (selectedLinePointIndex >= 0 && entity == selectedLinePointEntity){
                        // move the selected line point instead of the entity (the gizmo sits
                        // on the point, so newPos is its new world position)
                        LineRendererComponent* linesComp = target->findComponent<LineRendererComponent>(entity);
                        Transform* lineTransform = target->findComponent<Transform>(entity);
                        if (linesComp && lineTransform && selectedLinePointIndex < (int)getLinePointWorldPositions(*linesComp, *lineTransform).size()){
                            Vector3 newLocalPos = toLinePointSpace(*linesComp, *lineTransform, newPos);
                            dragCommand = new PropertyCmd<Vector3>(project, commandSceneId, entity,
                                ComponentType::LineRendererComponent, getLinePointProperty(*linesComp, selectedLinePointIndex), newLocalPos);
                        }
                    }else if (selectedPolygonPointIndex >= 0 && entity == selectedPolygonPointEntity){
                        // move the selected polygon vertex instead of the entity
                        Transform* polyTransform = target->findComponent<Transform>(entity);
                        if (polyTransform){
                            Vector3 newLocalPos = polyTransform->modelMatrix.inverse() * newPos;
                            std::string pointProp = "points[" + std::to_string(selectedPolygonPointIndex) + "].position";
                            dragCommand = new PropertyCmd<Vector3>(project, commandSceneId, entity,
                                selectedPolygonPointIsMesh ? ComponentType::MeshPolygonComponent : ComponentType::PolygonComponent,
                                pointProp, newLocalPos);
                        }
                    }else if (selectedJointAnchorIndex >= 0 && entity == selectedJointAnchorEntity){
                        // move the selected Joint 2D anchor (local to its body, or a world point)
                        if (Joint2DComponent* joint = target->findComponent<Joint2DComponent>(entity)){
                            const std::string anchorProp = getJointAnchorProperty(entity, selectedJointAnchorIndex);
                            if (!anchorProp.empty()){
                                const Vector2 value = joint2DAnchorFromWorld(target, *joint, selectedJointAnchorIndex == 1, newPos);
                                dragCommand = new PropertyCmd<Vector2>(project, commandSceneId, entity, ComponentType::Joint2DComponent, anchorProp, value);
                            }
                        }
                    }else if (selectedCapsuleHandleIndex >= 0 && entity == selectedCapsuleHandleEntity){
                        // move the selected capsule end or radius handle: the whole list is the
                        // property, so the drag is one undo step whichever capsule fields it changes
                        std::vector<CapsuleShadowShape> capsules;
                        if (capsulesWithHandleAt(entity, selectedCapsuleHandleIndex, newPos, capsuleDragBase, capsules)){
                            dragCommand = new PropertyCmd<std::vector<CapsuleShadowShape>>(project, commandSceneId, entity,
                                ComponentType::CapsuleShadowCasterComponent, "capsules", capsules);
                        }
                    }else{
                        Matrix4 gizmoMatrix = Matrix4::translateMatrix(newPos) * gizmoRMatrix * Matrix4::scaleMatrix(Vector3(1,1,1));
                        Matrix4 objMatrix = gizmoMatrix * objectMatrixOffset[entity];

                        applyObjectTransform(objMatrix);
                    }
                }

                if (tools.getGizmoSelected() == GizmoSelected::ROTATE){
                    Vector3 lastPoint = gizmoPosition - rretrun.point;

                    float dot = cursorStartOffset.dotProduct(lastPoint);
                    float slength = cursorStartOffset.length() * lastPoint.length();
                    float cosine = (slength != 0) ? dot / slength : 0;
                    cosine = std::fmax(-1.0, std::fmin(1.0, cosine));
                    float orig_angle = acos(cosine);

                    Vector3 cross = cursorStartOffset.crossProduct(lastPoint);
                    float sign = cross.dotProduct(cursorPlane.normal);

                    float angle = snapRotationAngle((sign < 0) ? -orig_angle : orig_angle, invertRotationSnap);

                    Quaternion newRot = Quaternion(Angle::radToDefault(angle), rotationAxis) * rotationStartOffset;

                    Matrix4 gizmoMatrix = Matrix4::translateMatrix(gizmoPosition) * newRot.getRotationMatrix() * Matrix4::scaleMatrix(Vector3(1,1,1));
                    Matrix4 objMatrix = gizmoMatrix * objectMatrixOffset[entity];

                    applyObjectTransform(objMatrix);
                }

                if (tools.getGizmoSelected() == GizmoSelected::SCALE){
                    Vector3 lastPoint = gizmoPosition - rretrun.point;

                    Vector3 startRotPoint = gizmoRMatrix.inverse() * cursorStartOffset;
                    Vector3 lastRotPoint = gizmoRMatrix.inverse() * lastPoint;

                    float radioX = (startRotPoint.x != 0) ? (lastRotPoint.x / startRotPoint.x) : 1;
                    float radioY = (startRotPoint.y != 0) ? (lastRotPoint.y / startRotPoint.y) : 1;
                    float radioZ = (startRotPoint.z != 0) ? (lastRotPoint.z / startRotPoint.z) : 1;

                    Vector3 newScale;
                    if (tools.getGizmoSideSelected() == GizmoSideSelected::XYZ){
                        newScale = Vector3((lastPoint.length() / cursorStartOffset.length()));
                    }else if (tools.getGizmoSideSelected() == GizmoSideSelected::X){
                        newScale = Vector3(radioX, 1, 1);
                    }else if(tools.getGizmoSideSelected() == GizmoSideSelected::Y){
                        newScale = Vector3(1, radioY, 1);
                    }else if(tools.getGizmoSideSelected() == GizmoSideSelected::Z){
                        newScale = Vector3(1, 1, radioZ);
                    }else if (tools.getGizmoSideSelected() == GizmoSideSelected::XY){
                        newScale = Vector3(radioX, radioY, 1);
                    }else if (tools.getGizmoSideSelected() == GizmoSideSelected::XZ){
                        newScale = Vector3(radioX, 1, radioZ);
                    }else if (tools.getGizmoSideSelected() == GizmoSideSelected::YZ){
                        newScale = Vector3(1, radioY, radioZ);
                    }

                    newScale = Vector3(abs(newScale.x), abs(newScale.y), abs(newScale.z));

                    Matrix4 gizmoMatrix = Matrix4::translateMatrix(gizmoPosition) * gizmoRMatrix * Matrix4::scaleMatrix(newScale);
                    Matrix4 objMatrix = gizmoMatrix * objectMatrixOffset[entity];

                    applyObjectTransform(objMatrix);
                }

                if (tools.getGizmoSelected() == GizmoSelected::OBJECT2D){
                    // Handle occluder polygon point sub-selection drag
                    if (selectedOccluderPointIndex >= 0 && entity == selectedOccluderPointEntity){
                        ShadowCaster2DComponent* occluder = target->findComponent<ShadowCaster2DComponent>(entity);
                        Transform* occTransform = target->findComponent<Transform>(entity);
                        if (occluder && occTransform && selectedOccluderPointIndex < (int)occluder->points.size()
                            && tools.getGizmo2DSideSelected() == Gizmo2DSideSelected::CENTER){

                            // new world position of the point, following the cursor
                            Vector3 newWorldPos = rretrun.point + cursorStartOffset;

                            if (displaySettings.snapToGrid) {
                                float spacing = displaySettings.gridSpacing2D;
                                if (spacing > 0.0f) {
                                    newWorldPos.x = std::round(newWorldPos.x / spacing) * spacing;
                                    newWorldPos.y = std::round(newWorldPos.y / spacing) * spacing;
                                }
                            }

                            Vector3 newLocalPos = occTransform->modelMatrix.inverse() * newWorldPos;

                            std::vector<Vector2> newPoints = occluder->points;
                            newPoints[selectedOccluderPointIndex] = Vector2(newLocalPos.x, newLocalPos.y);

                            dragCommand = new PropertyCmd<std::vector<Vector2>>(project, commandSceneId, entity,
                                ComponentType::ShadowCaster2DComponent, "points", newPoints);
                        }
                    }else
                    // Handle line endpoint sub-selection drag
                    if (selectedLinePointIndex >= 0 && entity == selectedLinePointEntity){
                        LineRendererComponent* linesComp = target->findComponent<LineRendererComponent>(entity);
                        Transform* lineTransform = target->findComponent<Transform>(entity);
                        const std::vector<Vector3> linePoints = (linesComp && lineTransform)
                            ? getLinePointWorldPositions(*linesComp, *lineTransform) : std::vector<Vector3>();
                        if (linesComp && lineTransform && selectedLinePointIndex < (int)linePoints.size()
                            && tools.getGizmo2DSideSelected() == Gizmo2DSideSelected::CENTER){

                            // new world position of the endpoint, following the cursor
                            Vector3 newWorldPos = rretrun.point + cursorStartOffset;

                            if (displaySettings.snapToGrid) {
                                float spacing = displaySettings.gridSpacing2D;
                                if (spacing > 0.0f) {
                                    newWorldPos.x = std::round(newWorldPos.x / spacing) * spacing;
                                    newWorldPos.y = std::round(newWorldPos.y / spacing) * spacing;
                                }
                            }

                            // the cursor plane is at the entity's Z: keep the point's own Z
                            const Vector3 oldLocalPos = toLinePointSpace(*linesComp, *lineTransform, linePoints[selectedLinePointIndex]);
                            Vector3 newLocalPos = toLinePointSpace(*linesComp, *lineTransform, newWorldPos);
                            newLocalPos.z = oldLocalPos.z;

                            dragCommand = new PropertyCmd<Vector3>(project, commandSceneId, entity,
                                ComponentType::LineRendererComponent, getLinePointProperty(*linesComp, selectedLinePointIndex), newLocalPos);
                        }
                    }else
                    // Handle polygon/mesh-polygon vertex sub-selection drag
                    if (selectedPolygonPointIndex >= 0 && entity == selectedPolygonPointEntity){
                        Transform* polyTransform = target->findComponent<Transform>(entity);
                        Vector3 worldOld;
                        bool hasPoint = getPolygonPointWorld(entity, selectedPolygonPointIsMesh, selectedPolygonPointIndex, worldOld);
                        if (polyTransform && hasPoint && tools.getGizmo2DSideSelected() == Gizmo2DSideSelected::CENTER){
                            // new world position of the vertex, following the cursor
                            Vector3 newWorldPos = rretrun.point + cursorStartOffset;

                            if (displaySettings.snapToGrid) {
                                float spacing = displaySettings.gridSpacing2D;
                                if (spacing > 0.0f) {
                                    newWorldPos.x = std::round(newWorldPos.x / spacing) * spacing;
                                    newWorldPos.y = std::round(newWorldPos.y / spacing) * spacing;
                                }
                            }

                            // the cursor plane is at the entity's Z: keep the vertex's own local Z
                            Matrix4 invModel = polyTransform->modelMatrix.inverse();
                            Vector3 oldLocal = invModel * worldOld;
                            Vector3 newLocalPos = invModel * newWorldPos;
                            newLocalPos.z = oldLocal.z;

                            std::string pointProp = "points[" + std::to_string(selectedPolygonPointIndex) + "].position";
                            dragCommand = new PropertyCmd<Vector3>(project, commandSceneId, entity,
                                selectedPolygonPointIsMesh ? ComponentType::MeshPolygonComponent : ComponentType::PolygonComponent,
                                pointProp, newLocalPos);
                        }
                    }else
                    // Handle Joint 2D anchor handle drag
                    if (selectedJointAnchorIndex >= 0 && entity == selectedJointAnchorEntity){
                        Joint2DComponent* joint = target->findComponent<Joint2DComponent>(entity);
                        const std::string anchorProp = getJointAnchorProperty(entity, selectedJointAnchorIndex);
                        if (joint && !anchorProp.empty() && tools.getGizmo2DSideSelected() == Gizmo2DSideSelected::CENTER){
                            Vector3 newWorldPos = rretrun.point + cursorStartOffset;

                            if (displaySettings.snapToGrid) {
                                float spacing = displaySettings.gridSpacing2D;
                                if (spacing > 0.0f) {
                                    newWorldPos.x = std::round(newWorldPos.x / spacing) * spacing;
                                    newWorldPos.y = std::round(newWorldPos.y / spacing) * spacing;
                                }
                            }

                            const Vector2 value = joint2DAnchorFromWorld(target, *joint, selectedJointAnchorIndex == 1, newWorldPos);
                            dragCommand = new PropertyCmd<Vector2>(project, commandSceneId, entity, ComponentType::Joint2DComponent, anchorProp, value);
                        }
                    }else
                    // Handle tile sub-selection drag
                    if (selectedTileIndex >= 0 && entity == selectedTileEntity){
                        TilemapComponent* tilemap = target->findComponent<TilemapComponent>(entity);
                        Transform* tileTransform = target->findComponent<Transform>(entity);
                        if (tilemap && tileTransform && selectedTileIndex < (int)tilemap->numTiles){
                            Vector3 delta = gizmoRMatrix.inverse() * ((rretrun.point + cursorStartOffset) - gizmoStartPosition);
                            Vector3 sizeDelta = gizmoRMatrix.inverse() * -(gizmoStartPosition - rretrun.point - cursorStartOffset);

                            // Transform delta to tilemap local space
                            Vector3 oScale = Vector3(1.0f, 1.0f, 1.0f);
                            oScale.x = Vector3(tileTransform->modelMatrix[0][0], tileTransform->modelMatrix[0][1], tileTransform->modelMatrix[0][2]).length();
                            oScale.y = Vector3(tileTransform->modelMatrix[1][0], tileTransform->modelMatrix[1][1], tileTransform->modelMatrix[1][2]).length();

                            Vector2 localDelta(delta.x / oScale.x, delta.y / oScale.y);
                            Vector2 localSizeDelta(sizeDelta.x / oScale.x, sizeDelta.y / oScale.y);

                            Vector2 newTilePos = tileStartPosition;
                            float newTileW = tileStartWidth;
                            float newTileH = tileStartHeight;
                            const float tileMaxX = tileStartPosition.x + tileStartWidth;
                            const float tileMaxY = tileStartPosition.y + tileStartHeight;

                            Gizmo2DSideSelected side = tools.getGizmo2DSideSelected();
                            if (side == Gizmo2DSideSelected::CENTER){
                                newTilePos = tileStartPosition + localDelta;
                            }else if (side == Gizmo2DSideSelected::NX){
                                newTilePos.x = tileStartPosition.x + localDelta.x;
                                newTileW = tileStartWidth - localSizeDelta.x;
                            }else if (side == Gizmo2DSideSelected::NY){
                                newTilePos.y = tileStartPosition.y + localDelta.y;
                                newTileH = tileStartHeight - localSizeDelta.y;
                            }else if (side == Gizmo2DSideSelected::PX){
                                newTileW = tileStartWidth + localSizeDelta.x;
                            }else if (side == Gizmo2DSideSelected::PY){
                                newTileH = tileStartHeight + localSizeDelta.y;
                            }else if (side == Gizmo2DSideSelected::NX_NY){
                                newTilePos.x = tileStartPosition.x + localDelta.x;
                                newTilePos.y = tileStartPosition.y + localDelta.y;
                                newTileW = tileStartWidth - localSizeDelta.x;
                                newTileH = tileStartHeight - localSizeDelta.y;
                            }else if (side == Gizmo2DSideSelected::NX_PY){
                                newTilePos.x = tileStartPosition.x + localDelta.x;
                                newTileW = tileStartWidth - localSizeDelta.x;
                                newTileH = tileStartHeight + localSizeDelta.y;
                            }else if (side == Gizmo2DSideSelected::PX_NY){
                                newTilePos.y = tileStartPosition.y + localDelta.y;
                                newTileW = tileStartWidth + localSizeDelta.x;
                                newTileH = tileStartHeight - localSizeDelta.y;
                            }else if (side == Gizmo2DSideSelected::PX_PY){
                                newTileW = tileStartWidth + localSizeDelta.x;
                                newTileH = tileStartHeight + localSizeDelta.y;
                            }

                            if (preserveAspectRatio && isCorner2DGizmoSide(side)) {
                                Vector2 lockedSize = lockObject2DAspectRatio(Vector2(tileStartWidth, tileStartHeight), Vector2(newTileW, newTileH));
                                newTileW = lockedSize.x;
                                newTileH = lockedSize.y;
                                newTilePos.x = gizmo2DSideUsesNegativeX(side) ? (tileMaxX - newTileW) : tileStartPosition.x;
                                newTilePos.y = gizmo2DSideUsesNegativeY(side) ? (tileMaxY - newTileH) : tileStartPosition.y;
                            }

                            if (displaySettings.snapTile) {
                                // Snap in local tilemap space to the tile's own size so
                                // neighboring tiles pack edge-to-edge when grouping.
                                float spacingX = tileStartWidth;
                                float spacingY = tileStartHeight;
                                if (spacingX > 0.0f && spacingY > 0.0f) {
                                    auto snapX = [&](float lx) {
                                        return std::round(lx / spacingX) * spacingX;
                                    };
                                    auto snapY = [&](float ly) {
                                        return std::round(ly / spacingY) * spacingY;
                                    };

                                    if (side == Gizmo2DSideSelected::CENTER ||
                                        side == Gizmo2DSideSelected::NX ||
                                        side == Gizmo2DSideSelected::NX_NY ||
                                        side == Gizmo2DSideSelected::NX_PY) {
                                        newTilePos.x = snapX(newTilePos.x);
                                    } else {
                                        float snappedFarX = snapX(newTilePos.x + newTileW);
                                        newTileW = snappedFarX - newTilePos.x;
                                    }
                                    if (side == Gizmo2DSideSelected::CENTER ||
                                        side == Gizmo2DSideSelected::NY ||
                                        side == Gizmo2DSideSelected::NX_NY ||
                                        side == Gizmo2DSideSelected::PX_NY) {
                                        newTilePos.y = snapY(newTilePos.y);
                                    } else {
                                        float snappedFarY = snapY(newTilePos.y + newTileH);
                                        newTileH = snappedFarY - newTilePos.y;
                                    }
                                }
                            } else if (displaySettings.snapToGrid) {
                                float spacing = displaySettings.gridSpacing2D;
                                if (spacing > 0.0f) {
                                    // Snap in world space so tiles align to the global grid
                                    // regardless of the tilemap entity's own world position/scale.
                                    // modelMatrix is [col][row]; column 3 holds world translation.
                                    float wX = tileTransform->modelMatrix[3][0];
                                    float wY = tileTransform->modelMatrix[3][1];
                                    float sX = (oScale.x > 0.0f) ? oScale.x : 1.0f;
                                    float sY = (oScale.y > 0.0f) ? oScale.y : 1.0f;

                                    // Convert a local X/Y value to/from world space and snap.
                                    auto snapX = [&](float lx) {
                                        return (std::round((lx * sX + wX) / spacing) * spacing - wX) / sX;
                                    };
                                    auto snapY = [&](float ly) {
                                        return (std::round((ly * sY + wY) / spacing) * spacing - wY) / sY;
                                    };

                                    if (side == Gizmo2DSideSelected::CENTER ||
                                        side == Gizmo2DSideSelected::NX ||
                                        side == Gizmo2DSideSelected::NX_NY ||
                                        side == Gizmo2DSideSelected::NX_PY) {
                                        newTilePos.x = snapX(newTilePos.x);
                                    } else {
                                        float snappedFarX = snapX(newTilePos.x + newTileW);
                                        newTileW = snappedFarX - newTilePos.x;
                                    }
                                    if (side == Gizmo2DSideSelected::CENTER ||
                                        side == Gizmo2DSideSelected::NY ||
                                        side == Gizmo2DSideSelected::NX_NY ||
                                        side == Gizmo2DSideSelected::PX_NY) {
                                        newTilePos.y = snapY(newTilePos.y);
                                    } else {
                                        float snappedFarY = snapY(newTilePos.y + newTileH);
                                        newTileH = snappedFarY - newTilePos.y;
                                    }
                                }
                            }

                            if (side == Gizmo2DSideSelected::CENTER){
                                if (newTilePos.x < 0) newTilePos.x = 0;
                                if (newTilePos.y < 0) newTilePos.y = 0;
                            }

                            if (side == Gizmo2DSideSelected::NX || side == Gizmo2DSideSelected::NX_NY || side == Gizmo2DSideSelected::NX_PY){
                                newTilePos.x = std::max(0.0f, std::min(newTilePos.x, tileMaxX - 1.0f));
                                newTileW = tileMaxX - newTilePos.x;
                            }

                            if (side == Gizmo2DSideSelected::NY || side == Gizmo2DSideSelected::NX_NY || side == Gizmo2DSideSelected::PX_NY){
                                newTilePos.y = std::max(0.0f, std::min(newTilePos.y, tileMaxY - 1.0f));
                                newTileH = tileMaxY - newTilePos.y;
                            }

                            if (newTileW < 1) newTileW = 1;
                            if (newTileH < 1) newTileH = 1;

                            if (side != Gizmo2DSideSelected::NONE){
                                std::string prefix = "tiles[" + std::to_string(selectedTileIndex) + "]";
                                MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                                multiCmd->addPropertyCmd<Vector2>(project, commandSceneId, entity, ComponentType::TilemapComponent,
                                    prefix + ".position", newTilePos);
                                multiCmd->addPropertyCmd<float>(project, commandSceneId, entity, ComponentType::TilemapComponent,
                                    prefix + ".width", newTileW);
                                multiCmd->addPropertyCmd<float>(project, commandSceneId, entity, ComponentType::TilemapComponent,
                                    prefix + ".height", newTileH);
                                dragCommand = multiCmd;
                            }
                        }
                    }else if (isInstanceDrag){
                        // Instance CENTER drag (position only) for OBJECT2D
                        if (tools.getGizmo2DSideSelected() == Gizmo2DSideSelected::CENTER){
                            Vector3 newPos = gizmoRMatrix.inverse() * ((rretrun.point + cursorStartOffset) - gizmoStartPosition);
                            newPos = gizmoStartPosition + (gizmoRMatrix * newPos);
                            if (displaySettings.snapToGrid) {
                                float spacing = displaySettings.gridSpacing2D;
                                if (spacing > 0.0f) {
                                    newPos.x = std::round(newPos.x / spacing) * spacing;
                                    newPos.y = std::round(newPos.y / spacing) * spacing;
                                }
                            }
                            Matrix4 gizmoMatrix = Matrix4::translateMatrix(newPos) * gizmoRMatrix * Matrix4::scaleMatrix(Vector3(1,1,1));
                            Matrix4 objMatrix = gizmoMatrix * objectMatrixOffset[entity];
                            emitInstanceTransformCmd(objMatrix);
                        }
                    }else{
                    // Original entity-level OBJECT2D handling
                    bool isSprite = target->getComponentArray<SpriteComponent>()->hasEntity(entity);
                    bool isTilemap = target->getComponentArray<TilemapComponent>()->hasEntity(entity);
                    bool isLayout = target->getComponentArray<RectTransformComponent>()->hasEntity(entity);
                    bool isText = target->getComponentArray<TextComponent>()->hasEntity(entity);

                    Gizmo2DSideSelected side = tools.getGizmo2DSideSelected();
                    Vector3 deltaPos = gizmoRMatrix.inverse() * ((rretrun.point + cursorStartOffset) - gizmoStartPosition);
                    Vector3 deltaSize = gizmoRMatrix.inverse() * -(gizmoStartPosition - rretrun.point - cursorStartOffset);

                    Vector3 oScale = getMatrixScale(transform->modelMatrix);
                    if (oScale.x == 0.0f) oScale.x = 1.0f;
                    if (oScale.y == 0.0f) oScale.y = 1.0f;
                    Vector3 posOffset = Vector3::ZERO;
                    Vector2 sizeDeltaLocal = Vector2::ZERO;

                    if (side == Gizmo2DSideSelected::CENTER){
                        posOffset = deltaPos;
                    }else if (side == Gizmo2DSideSelected::NX){
                        posOffset.x = deltaPos.x;
                        sizeDeltaLocal.x = -deltaSize.x / oScale.x;
                    }else if (side == Gizmo2DSideSelected::NY){
                        posOffset.y = deltaPos.y;
                        sizeDeltaLocal.y = -deltaSize.y / oScale.y;
                    }else if (side == Gizmo2DSideSelected::PX){
                        sizeDeltaLocal.x = deltaSize.x / oScale.x;
                    }else if (side == Gizmo2DSideSelected::PY){
                        sizeDeltaLocal.y = deltaSize.y / oScale.y;
                    }else if (side == Gizmo2DSideSelected::NX_NY){
                        posOffset.x = deltaPos.x;
                        posOffset.y = deltaPos.y;
                        sizeDeltaLocal.x = -deltaSize.x / oScale.x;
                        sizeDeltaLocal.y = -deltaSize.y / oScale.y;
                    }else if (side == Gizmo2DSideSelected::NX_PY){
                        posOffset.x = deltaPos.x;
                        sizeDeltaLocal.x = -deltaSize.x / oScale.x;
                        sizeDeltaLocal.y = deltaSize.y / oScale.y;
                    }else if (side == Gizmo2DSideSelected::PX_NY){
                        posOffset.y = deltaPos.y;
                        sizeDeltaLocal.x = deltaSize.x / oScale.x;
                        sizeDeltaLocal.y = -deltaSize.y / oScale.y;
                    }else if (side == Gizmo2DSideSelected::PX_PY){
                        sizeDeltaLocal.x = deltaSize.x / oScale.x;
                        sizeDeltaLocal.y = deltaSize.y / oScale.y;
                    }

                    if (preserveAspectRatio && isCorner2DGizmoSide(side)){
                        Vector2 freeformSizeDeltaLocal = sizeDeltaLocal;
                        Vector2 lockedSize = lockObject2DAspectRatio(objectSizeOffset[entity], objectSizeOffset[entity] + sizeDeltaLocal);
                        sizeDeltaLocal = lockedSize - objectSizeOffset[entity];
                        if (gizmo2DSideUsesNegativeX(side)){
                            posOffset.x = (freeformSizeDeltaLocal.x != 0.0f) ? (posOffset.x * (sizeDeltaLocal.x / freeformSizeDeltaLocal.x)) : 0.0f;
                        }
                        if (gizmo2DSideUsesNegativeY(side)){
                            posOffset.y = (freeformSizeDeltaLocal.y != 0.0f) ? (posOffset.y * (sizeDeltaLocal.y / freeformSizeDeltaLocal.y)) : 0.0f;
                        }
                    }

                    Vector3 newPos = gizmoStartPosition + (gizmoRMatrix * posOffset);

                    if (side == Gizmo2DSideSelected::CENTER){
                        // Snap absolute world position to grid (must be done after newPos becomes world-space)
                        if (displaySettings.snapToGrid) {
                            float spacing = displaySettings.gridSpacing2D;
                            if (spacing > 0.0f) {
                                newPos.x = std::round(newPos.x / spacing) * spacing;
                                newPos.y = std::round(newPos.y / spacing) * spacing;
                            }
                        }
                    }

                    // S41 R21 Pixel Move Snap: the position on whole art pixels (Ctrl held: free)
                    if (pixelMoveSnap){
                        newPos.x = jasperengine::pixelart::snapWorld(newPos.x, Engine::getPixelsPerUnit());
                        newPos.y = jasperengine::pixelart::snapWorld(newPos.y, Engine::getPixelsPerUnit());
                    }

                    Matrix4 gizmoMatrix = Matrix4::translateMatrix(newPos) * gizmoRMatrix * Matrix4::scaleMatrix(Vector3(1,1,1));
                    Matrix4 objMatrix = gizmoMatrix * objectMatrixOffset[entity];

                    if (transformParent){
                        objMatrix = transformParent->modelMatrix.inverse() * objMatrix;
                    }

                    // a sprite's and a tilemap's sizes are pixels: the drag's world units times their
                    // Pixels Per Unit (S41 R6), whole pixels under Pixel Move Snap
                    float sizePixelsPerUnit = 1.0f;
                    if (SpriteComponent* sprite = target->findComponent<SpriteComponent>(entity))
                        sizePixelsPerUnit = resolvePixelsPerUnit(sprite->pixelsPerUnit);
                    else if (TilemapComponent* tilemapSize = target->findComponent<TilemapComponent>(entity))
                        sizePixelsPerUnit = resolvePixelsPerUnit(tilemapSize->pixelsPerUnit);
                    Vector2 size = objectSizeOffset[entity] + sizeDeltaLocal * sizePixelsPerUnit;
                    if (pixelMoveSnap)
                        size = Vector2(std::round(size.x), std::round(size.y));
                    Vector3 pos = Vector3(objMatrix[3][0], objMatrix[3][1], objMatrix[3][2]);

                    if (size.x < 0) size.x = 0;
                    if (size.y < 0) size.y = 0;

                    if (isLayout){
                        // the matrix origin of a pivoted UI element is not its position
                        RectTransformComponent& layout = target->getComponent<RectTransformComponent>(entity);
                        pos -= getUIPivotShift(layout.pivot, size, transform->rotation, transform->scale);
                    }

                    if (isText){
                        TextComponent& text = target->getComponent<TextComponent>(entity);
                        if (text.pivotCentered){
                            float widthDelta = size.x - objectSizeOffset[entity].x;
                            Gizmo2DSideSelected side = tools.getGizmo2DSideSelected();
                            if (side == Gizmo2DSideSelected::NX || side == Gizmo2DSideSelected::NX_NY || side == Gizmo2DSideSelected::NX_PY) {
                                pos.x += widthDelta * 0.5f;
                            }else if (side == Gizmo2DSideSelected::PX || side == Gizmo2DSideSelected::PX_NY || side == Gizmo2DSideSelected::PX_PY) {
                                pos.x += widthDelta * 0.5f;
                            }
                        }
                    }

                    if (side != Gizmo2DSideSelected::NONE){
                        MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                        if (isLayout){
                            multiCmd->addPropertyCmd<unsigned int>(project, commandSceneId, entity, ComponentType::RectTransformComponent, "width", static_cast<unsigned int>(size.x));
                            multiCmd->addPropertyCmd<unsigned int>(project, commandSceneId, entity, ComponentType::RectTransformComponent, "height", static_cast<unsigned int>(size.y));
                            if (isText){
                                if (side == Gizmo2DSideSelected::NX || side == Gizmo2DSideSelected::PX || 
                                    side == Gizmo2DSideSelected::NX_NY || side == Gizmo2DSideSelected::NX_PY || 
                                    side == Gizmo2DSideSelected::PX_NY || side == Gizmo2DSideSelected::PX_PY) {
                                    multiCmd->addPropertyCmd<bool>(project, commandSceneId, entity, ComponentType::TextComponent, "fixedWidth", true);
                                }
                                if (side == Gizmo2DSideSelected::NY || side == Gizmo2DSideSelected::PY || 
                                    side == Gizmo2DSideSelected::NX_NY || side == Gizmo2DSideSelected::NX_PY || 
                                    side == Gizmo2DSideSelected::PX_NY || side == Gizmo2DSideSelected::PX_PY) {
                                    multiCmd->addPropertyCmd<bool>(project, commandSceneId, entity, ComponentType::TextComponent, "fixedHeight", true);
                                }
                            }
                        }else if (isSprite){
                            multiCmd->addPropertyCmd<unsigned int>(project, commandSceneId, entity, ComponentType::SpriteComponent, "width", static_cast<unsigned int>(size.x));
                            multiCmd->addPropertyCmd<unsigned int>(project, commandSceneId, entity, ComponentType::SpriteComponent, "height", static_cast<unsigned int>(size.y));
                        }else if (isTilemap){
                            multiCmd->addPropertyCmd<unsigned int>(project, commandSceneId, entity, ComponentType::TilemapComponent, "width", static_cast<unsigned int>(size.x));
                            multiCmd->addPropertyCmd<unsigned int>(project, commandSceneId, entity, ComponentType::TilemapComponent, "height", static_cast<unsigned int>(size.y));
                        }
                        if (isLayout && anchoredMoveStart.count(entity)){
                            // placed by its anchor preset: the move goes to positionOffset
                            multiCmd->addPropertyCmd<Vector2>(project, commandSceneId, entity, ComponentType::RectTransformComponent,
                                "positionOffset", getAnchoredPositionOffset(target, entity, pos, size));
                        }else{
                            multiCmd->addPropertyCmd<Vector3>(project, commandSceneId, entity, ComponentType::Transform, "position", pos);
                        }
                        dragCommand = multiCmd;
                    }
                    } // end else (entity-level OBJECT2D)
                }

                // a move Record does not key (2D gizmo, UI layout, instances, sub-points) of a
                // GameObject it animates: say so once per drag
                if (dragCommand && !gizmoRecordNoted && ClipRecordHooks::claims(commandSceneId, entity, ClipChannel::Position)){
                    gizmoRecordNoted = true;
                    ClipRecordHooks::note("Record does not key 2D, UI layout, instance or point moves yet: this drag is a scene step");
                }
                if (dragCommand){
                    pushDragCommand(sceneId, dragCommand);
                }
            }
        }
    }
}

// positionOffset that puts entity at localPosition (in its parent's space) once it is size
// big: the offset lives in the space of its nearest RectTransform ancestor (UISystem's
// layout space), the canvas at the root, and the preset moves the corner as the size changes
Vector2 editor::SceneRender::getAnchoredPositionOffset(Scene* target, Entity entity, const Vector3& localPosition, const Vector2& size) const{
    auto start = anchoredMoveStart.find(entity);
    const RectTransformComponent* layout = target->findComponent<RectTransformComponent>(entity);
    if (start == anchoredMoveStart.end() || !layout){
        return Vector2::ZERO;
    }

    // the corner offset UISystem::applyAnchorPreset gives a size between two anchor points
    auto presetCornerOffset = [](float anchorLow, float anchorHigh, float extent) -> float {
        if (anchorLow != anchorHigh || anchorLow == 0.0f){
            return 0.0f;
        }
        return (anchorLow == 1.0f) ? -extent : -std::floor(extent / 2.0f);
    };
    auto startSizeIt = objectSizeOffset.find(entity);
    const Vector2 startSize = (startSizeIt != objectSizeOffset.end()) ? startSizeIt->second : size;
    const Vector2 presetShift(
        presetCornerOffset(layout->anchorPointLeft, layout->anchorPointRight, std::floor(size.x)) -
            presetCornerOffset(layout->anchorPointLeft, layout->anchorPointRight, std::floor(startSize.x)),
        presetCornerOffset(layout->anchorPointTop, layout->anchorPointBottom, std::floor(size.y)) -
            presetCornerOffset(layout->anchorPointTop, layout->anchorPointBottom, std::floor(startSize.y)));

    Matrix4 parentToWorld;
    Matrix4 layoutToWorld;
    const Transform& transform = target->getComponent<Transform>(entity);
    if (transform.parent != NULL_ENTITY){
        if (const Transform* parentTransform = target->findComponent<Transform>(transform.parent)){
            parentToWorld = parentTransform->modelMatrix;
        }
        layoutToWorld = parentToWorld;
        for (Entity ancestor = transform.parent; ancestor != NULL_ENTITY; ){
            const Transform* ancestorTransform = target->findComponent<Transform>(ancestor);
            if (target->findComponent<RectTransformComponent>(ancestor)){
                if (ancestorTransform){
                    layoutToWorld = ancestorTransform->modelMatrix;
                }
                break;
            }
            ancestor = ancestorTransform ? ancestorTransform->parent : NULL_ENTITY;
        }
    }

    Matrix4 worldToLayout = layoutToWorld.inverse();
    if (!worldToLayout.isValid()){
        worldToLayout = Matrix4();
    }
    const Matrix4 parentToLayout = worldToLayout * parentToWorld;
    const Vector3 from = parentToLayout * start->second.second;
    const Vector3 to = parentToLayout * localPosition;
    return start->second.first + Vector2(to.x - from.x, to.y - from.y) - presetShift;
}

bool editor::SceneRender::isAnyGizmoSideSelected() const{
    const ToolsLayer& tools = gizmoTools();
    return (tools.getGizmoSideSelected() != editor::GizmoSideSelected::NONE || tools.getGizmo2DSideSelected() != Gizmo2DSideSelected::NONE);
}

// a Scene view brush holds the left button: Paint Dye, or the nav-grid brush (S19)
bool editor::SceneRender::isFluidPainting() const{
    return FluidEditorTools::isPainting(scene) || NavigationEditor::get().isBrushPainting(scene) ||
           NavigationEditor::get().isDraggingLinkHandle();
}

bool editor::SceneRender::isTerrainEditing() const{
    if (TerrainEditWindow* terrainEditWindow = Backend::getApp().getTerrainEditWindow()){
        return terrainEditWindow->isEditingScene(scene);
    }
    return false;
}

bool editor::SceneRender::hasToolSubSelection(const std::vector<Entity>& selEntities){
    if (selEntities.size() != 1){
        return false;
    }
    const Entity entity = selEntities[0];
    return (selectedTileIndex >= 0 && entity == selectedTileEntity)
        || (selectedInstanceIndex >= 0 && entity == selectedInstanceEntity)
        || (selectedOccluderPointIndex >= 0 && entity == selectedOccluderPointEntity)
        || (selectedLinePointIndex >= 0 && entity == selectedLinePointEntity)
        || (selectedPolygonPointIndex >= 0 && entity == selectedPolygonPointEntity)
        || (selectedJointAnchorIndex >= 0 && entity == selectedJointAnchorEntity)
        || (selectedCapsuleHandleIndex >= 0 && entity == selectedCapsuleHandleEntity)
        || (selectedTrackPointIndex >= 0 && entity == selectedTrackPointEntity)
        || (getPointEditLattice(selEntities) == entity && getLatticeSceneId() != NULL_PROJECT_SCENE &&
            !LatticeEditorOps::getSelectedPoints(getLatticeSceneId(), entity).empty())
        || (getKnotEditSpline(selEntities) == entity && !SplineEditor::get().getSelection().empty());
}

void editor::SceneRender::physicsGizmoDrag(PhysicsPlacementWindow* physics, uint32_t sceneId, bool invertRotationSnap){
    using GestureInput = PhysicsPlacementWindow::GestureInput;
    ToolsLayer& tools = toolslayer;
    const GizmoSideSelected side = tools.getGizmoSideSelected();

    if (!physicsDragBegun){
        physicsDragBegun = true;
        // off the handles the drag is a box select (drawn by the caller)
        if (side == GizmoSideSelected::NONE){
            return;
        }
        PlacementGesture gesture = PlacementGesture::Move;
        if (tools.getGizmoSelected() == GizmoSelected::ROTATE){
            gesture = PlacementGesture::Rotate;
        }else if (tools.getGizmoSelected() == GizmoSelected::SCALE){
            gesture = PlacementGesture::GatherSpread;
        }
        physicsGestureStarted = physics->beginGesture(sceneId, gesture, GestureInput::Gizmo);
        if (PlacementSession* session = physicsGestureStarted ? physics->heldGesture(sceneId, GestureInput::Gizmo) : nullptr){
            physicsMoversStart = session->getMoversCenter();
            physicsGizmoOffset = gizmoStartPosition - physicsMoversStart;
        }
    }

    // nothing moves after a refusal, or once Esc or Ctrl+Z cancelled the gesture
    PlacementSession* session = physicsGestureStarted ? physics->heldGesture(sceneId, GestureInput::Gizmo) : nullptr;
    RayReturn hit = mouseRay.intersects(cursorPlane);
    if (!session || side == GizmoSideSelected::NONE || !hit){
        return;
    }
    tools.mouseDrag(hit.point);

    // the handle's frame from the click: the gizmo turns with the objects in Local mode
    const Matrix4 handleRotation = rotationStartOffset.getRotationMatrix();

    if (tools.getGizmoSelected() == GizmoSelected::TRANSLATE){
        Vector3 deltaPos = handleRotation.inverse() * ((hit.point + cursorStartOffset) - gizmoStartPosition);
        if (displaySettings.snapToGrid && displaySettings.gridSpacing3D > 0.0f){
            const float spacing = displaySettings.gridSpacing3D;
            deltaPos.x = std::round(deltaPos.x / spacing) * spacing;
            deltaPos.y = std::round(deltaPos.y / spacing) * spacing;
            deltaPos.z = std::round(deltaPos.z / spacing) * spacing;
        }
        Vector3 axes(1.0f, 1.0f, 1.0f);
        switch (side){
            case GizmoSideSelected::X: axes = Vector3(1, 0, 0); break;
            case GizmoSideSelected::Y: axes = Vector3(0, 1, 0); break;
            case GizmoSideSelected::Z: axes = Vector3(0, 0, 1); break;
            case GizmoSideSelected::XY: axes = Vector3(1, 1, 0); break;
            case GizmoSideSelected::XZ: axes = Vector3(1, 0, 1); break;
            case GizmoSideSelected::YZ: axes = Vector3(0, 1, 1); break;
            default: break;
        }
        const Vector3 handleDelta = handleRotation * Vector3(deltaPos.x * axes.x, deltaPos.y * axes.y, deltaPos.z * axes.z);
        // the movers' centre goes where the handle went
        session->setMoveTarget(physicsMoversStart + handleDelta);
    }else if (tools.getGizmoSelected() == GizmoSelected::ROTATE){
        // measured about where the gizmo was clicked, not where the objects have got to
        const Vector3 lastPoint = gizmoStartPosition - hit.point;
        const float dot = cursorStartOffset.dotProduct(lastPoint);
        const float lengths = cursorStartOffset.length() * lastPoint.length();
        const float cosine = std::fmax(-1.0f, std::fmin(1.0f, lengths != 0.0f ? dot / lengths : 0.0f));
        const float sign = cursorStartOffset.crossProduct(lastPoint).dotProduct(cursorPlane.normal);
        const float angle = snapRotationAngle(sign < 0.0f ? -std::acos(cosine) : std::acos(cosine), invertRotationSnap);
        session->setRotateDelta(Quaternion(Angle::radToDefault(angle), rotationAxis));
    }else if (tools.getGizmoSelected() == GizmoSelected::SCALE){
        const Vector3 lastPoint = gizmoStartPosition - hit.point;
        const Vector3 startLocal = handleRotation.inverse() * cursorStartOffset;
        const Vector3 lastLocal = handleRotation.inverse() * lastPoint;
        const float ratioX = startLocal.x != 0.0f ? lastLocal.x / startLocal.x : 1.0f;
        const float ratioY = startLocal.y != 0.0f ? lastLocal.y / startLocal.y : 1.0f;
        const float ratioZ = startLocal.z != 0.0f ? lastLocal.z / startLocal.z : 1.0f;
        Vector3 scale(1.0f, 1.0f, 1.0f);
        switch (side){
            case GizmoSideSelected::XYZ:
                scale = Vector3(cursorStartOffset.length() != 0.0f ? lastPoint.length() / cursorStartOffset.length() : 1.0f);
                break;
            case GizmoSideSelected::X: scale = Vector3(ratioX, 1, 1); break;
            case GizmoSideSelected::Y: scale = Vector3(1, ratioY, 1); break;
            case GizmoSideSelected::Z: scale = Vector3(1, 1, ratioZ); break;
            case GizmoSideSelected::XY: scale = Vector3(ratioX, ratioY, 1); break;
            case GizmoSideSelected::XZ: scale = Vector3(ratioX, 1, ratioZ); break;
            case GizmoSideSelected::YZ: scale = Vector3(1, ratioY, ratioZ); break;
            default: break;
        }
        // < 1 gathers, > 1 spreads; past twice the spread or through the centre means nothing more
        auto clampScale = [](float value){
            return std::isfinite(value) ? std::clamp(std::fabs(value), 0.0f, 2.0f) : 1.0f;
        };
        session->setGatherScale(Vector3(clampScale(scale.x), clampScale(scale.y), clampScale(scale.z)));
    }
}

void editor::SceneRender::updatePhysicsOverlay(PhysicsPlacementWindow* physics, uint32_t sceneId){
    PlacementOverlay overlay;
    if (editLayer.active || !physics || !physics->collectOverlay(sceneId, overlay)){
        physicsLines->setVisible(false);
        return;
    }

    const Vector4 regionColor(0.85f, 0.88f, 0.95f, 0.30f);
    const Vector4 slowingColor(1.00f, 0.42f, 0.15f, 1.0f);
    const Vector4 freeColor(0.35f, 0.90f, 0.45f, 1.0f);
    const Vector4 ringColor(1.00f, 0.72f, 0.25f, 0.9f);
    const Vector4 ringExceededColor(1.00f, 0.22f, 0.18f, 1.0f);
    const Vector4 ghostColor(0.78f, 0.82f, 0.92f, 0.55f);
    const Vector4 proxyColor(0.35f, 0.80f, 1.00f, 0.85f);
    const Vector4 axisColors[3] = {Vector4(0.92f, 0.32f, 0.30f, 1.0f), Vector4(0.40f, 0.88f, 0.35f, 1.0f), Vector4(0.38f, 0.55f, 1.00f, 1.0f)};

    physicsLines->clearLines();
    const Vector3 cameraPosition = camera->getWorldPosition();
    auto towardCamera = [&cameraPosition](const Vector3& point){
        const Vector3 direction = point - cameraPosition;
        return direction.length() > 1e-6f ? direction : Vector3(0.0f, 0.0f, 1.0f);
    };
    auto addBox = [&](const OBB& box, const Matrix4* transform, const Vector4& color){
        for (const auto& edge : BOX_EDGES){
            Vector3 a = box.getCorner(edge[0]);
            Vector3 b = box.getCorner(edge[1]);
            if (transform){
                a = *transform * a;
                b = *transform * b;
            }
            physicsLines->addLine(a, b, color);
        }
    };

    // the context region, while a gesture or a settle runs
    if (overlay.active && !overlay.region.isNull() && !overlay.region.isInfinite()){
        addBox(OBB(overlay.region), nullptr, regionColor);
    }

    // contacts: a dot a few pixels across, coloured by whether the mover is slowed there
    for (size_t i = 0; i < overlay.contactPoints.size(); i++){
        const Vector3& point = overlay.contactPoints[i];
        const bool slowing = i < overlay.contactSlowing.size() && overlay.contactSlowing[i];
        const float radius = billboardScreenScale(point, 3.0f);
        LineDrawUtils::addRing3D(physicsLines, point, towardCamera(point), radius, slowing ? slowingColor : freeColor, 8);
        LineDrawUtils::addRing3D(physicsLines, point, towardCamera(point), radius * 0.45f, slowing ? slowingColor : freeColor, 6);
    }

    // the teleport ring: dashed, facing the view, red once the handle is past it
    if (overlay.showTeleportRing && overlay.teleportRadius > 0.0f){
        const Vector4 color = overlay.teleportExceeded ? ringExceededColor : ringColor;
        auto [axis, right, forward] = LineDrawUtils::makeBasis(towardCamera(overlay.teleportCenter));
        (void)axis;
        const int segments = 64;
        for (int i = 0; i < segments; i += 2){
            const float a0 = (2.0f * (float)M_PI * (float)i) / (float)segments;
            const float a1 = (2.0f * (float)M_PI * (float)(i + 1)) / (float)segments;
            physicsLines->addLine(overlay.teleportCenter + (right * std::cos(a0) + forward * std::sin(a0)) * overlay.teleportRadius,
                                  overlay.teleportCenter + (right * std::cos(a1) + forward * std::sin(a1)) * overlay.teleportRadius, color);
        }
    }

    // ghosts: each mover's outline where it was before the gesture (where a cancel puts it)
    for (const PlacementOverlay::Ghost& ghost : overlay.ghosts){
        const Transform* transform = scene->findComponent<Transform>(ghost.entity);
        if (!transform){
            continue;
        }
        const OBB box = getFamilyVisualOBB(scene, ghost.entity, 0.0f);
        if (box.isNull()){
            continue;
        }
        const Matrix4 current = Matrix4::translateMatrix(transform->worldPosition) * transform->worldRotation.getRotationMatrix();
        const Matrix4 before = Matrix4::translateMatrix(ghost.position) * ghost.rotation.getRotationMatrix();
        const Matrix4 toBefore = before * current.inverse();
        addBox(box, &toBefore, ghostColor);
    }

    // the generated colliders (Gizmos > Physics proxies)
    for (size_t i = 0; i + 1 < overlay.proxySegments.size(); i += 2){
        physicsLines->addLine(overlay.proxySegments[i], overlay.proxySegments[i + 1], proxyColor);
    }

    // distance rays from the idle selection to its surroundings, with a tick at the hit;
    // their labels are the Scene view's (ImGui)
    for (const PlacementOverlay::Distance& distance : overlay.distances){
        const Vector4& color = axisColors[std::clamp(distance.axis, 0, 2)];
        physicsLines->addLine(distance.from, distance.to, color);
        LineDrawUtils::addRing3D(physicsLines, distance.to, towardCamera(distance.to), billboardScreenScale(distance.to, 2.5f), color, 6);
    }

    physicsLines->setVisible(physicsLines->getNumLines() > 0);
}

bool editor::SceneRender::worldToScreen(const Vector3& world, Vector2& screen) const{
    const CameraComponent& cameraComponent = scene->getComponent<CameraComponent>(camera->getEntity());
    const Vector4 clip = cameraComponent.viewProjectionMatrix * Vector4(world.x, world.y, world.z, 1.0f);
    if (clip.w <= 1e-6f){
        return false;
    }
    const float width = (viewWidth > 0) ? (float)viewWidth : (float)Engine::getCanvasWidth();
    const float height = (viewHeight > 0) ? (float)viewHeight : (float)Engine::getCanvasHeight();
    // the inverse of screenToRayFromMatrix
    screen.x = (clip.x / clip.w + 1.0f) * 0.5f * width;
    screen.y = (1.0f - clip.y / clip.w) * 0.5f * height;
    return std::isfinite(screen.x) && std::isfinite(screen.y);
}

bool editor::SceneRender::hasHoverFeedback() const{
    // the Paint Dye brush's circle follows the pointer like the terrain brush's
    return gizmoTools().isGizmoVisible() || isTerrainEditing() || isFluidPainting();
}

float editor::SceneRender::snapRotationAngle(float angle, bool invertRotationSnap) const{
    bool snapRotation = displaySettings.snapRotation;
    if (invertRotationSnap){
        snapRotation = !snapRotation;
    }

    if (!snapRotation || displaySettings.rotationSnapDegrees <= 0.0f){
        return angle;
    }

    float snapStep = Angle::defaultToRad(displaySettings.rotationSnapDegrees);
    return std::round(angle / snapStep) * snapStep;
}

void editor::SceneRender::updateTerrainBrushCursor(){
    if (!terrainBrushLines){
        return;
    }

    TerrainEditWindow* terrainEditWindow = Backend::getApp().getTerrainEditWindow();
    TerrainBrushCursor cursor;
    if (!terrainEditWindow || !terrainEditWindow->updateCursor(scene, ownerMouseRay, cursor) || !cursor.visible || cursor.outerPoints.size() < 2){
        terrainBrushLines->setVisible(false);
        return;
    }

    terrainBrushLines->clearLines();
    auto addLoop = [&](const std::vector<Vector3>& points, const Vector4& color){
        if (points.size() < 2){
            return;
        }
        for (size_t i = 0; i < points.size(); i++){
            terrainBrushLines->addLine(points[i], points[(i + 1) % points.size()], color);
        }
    };

    // Brush boundary plus the (dimmer) half-strength falloff contour, both draped
    // over the terrain surface by updateCursor.
    addLoop(cursor.outerPoints, Vector4(1.0f, 0.78f, 0.22f, 1.0f));
    addLoop(cursor.innerPoints, Vector4(0.75f, 0.58f, 0.16f, 1.0f));

    terrainBrushLines->setVisible(true);
}

bool editor::SceneRender::getFluidBrushCircle(std::vector<Vector2>& points){
    points.clear();
    std::vector<Vector3> world;
    if (!FluidEditorTools::cursorCircle(scene, ownerMouseRay, world) || world.size() < 2){
        return false;
    }

    points.reserve(world.size());
    for (const Vector3& point : world){
        Vector2 projected;
        if (!projectToView(point, projected)){
            points.clear();
            return false;
        }
        points.push_back(projected);
    }
    return true;
}

bool editor::SceneRender::projectToView(const Vector3& world, Vector2& point){
    // through the editor camera the pointer's ray comes from (screenToRay), into its pixels
    const float width = (viewWidth > 0) ? (float)viewWidth : (float)Engine::getCanvasWidth();
    const float height = (viewHeight > 0) ? (float)viewHeight : (float)Engine::getCanvasHeight();
    const CameraComponent& cameraComponent = scene->getComponent<CameraComponent>(camera->getEntity());
    const Vector4 clip = cameraComponent.viewProjectionMatrix * Vector4(world.x, world.y, world.z, 1.0f);
    if (clip.w <= 0.0f){
        return false;
    }
    point = Vector2((clip.x / clip.w + 1.0f) * 0.5f * width, (1.0f - clip.y / clip.w) * 0.5f * height);
    return true;
}

TextureRender& editor::SceneRender::getTexture(){
    return framebuffer.getRender().getColorTexture();
}

Ray editor::SceneRender::screenToRay(float x, float y){
    // before the view is first sized, the Engine's view is the one it draws at
    const float width = (viewWidth > 0) ? (float)viewWidth : (float)Engine::getCanvasWidth();
    const float height = (viewHeight > 0) ? (float)viewHeight : (float)Engine::getCanvasHeight();
    return screenToRayFromCamera(scene->getComponent<CameraComponent>(camera->getEntity()), x, y, width, height);
}

Camera* editor::SceneRender::getCamera(){
    return camera;
}

bool editor::SceneRender::isPreviewCameraUsable(Entity entity){
    return entity != NULL_ENTITY
        && scene
        && scene->isEntityCreated(entity)
        && scene->findComponent<CameraComponent>(entity)
        && scene->findComponent<Transform>(entity);
}

Entity editor::SceneRender::getActiveCameraEntity(){
    if (gameViewCameraEntity != NULL_ENTITY && isPreviewCameraUsable(gameViewCameraEntity)){
        return gameViewCameraEntity;
    }

    // isPreviewCameraActive() self-heals a stale preview entity, so this falls back
    // to the editor camera whenever the preview is no longer usable.
    if (isPreviewCameraActive()){
        return previewCameraEntity;
    }

    return camera->getEntity();
}

void editor::SceneRender::syncSceneCamera(){
    Entity activeCamera = getActiveCameraEntity();
    if (scene && activeCamera != NULL_ENTITY && scene->getCamera() != activeCamera){
        scene->setCamera(activeCamera);
    }
}

bool editor::SceneRender::setPreviewCamera(Entity entity){
    if (!isPreviewCameraUsable(entity)){
        return false;
    }

    previewCameraEntity = entity;
    if (!isPlaying){
        syncSceneCamera();
    }
    // Gizmos are hidden by update() every frame while the preview is active, which
    // runs before the scene is drawn, so there's no need to hide them here too.
    return true;
}

void editor::SceneRender::clearPreviewCamera(){
    previewCameraEntity = NULL_ENTITY;
    if (!isPlaying){
        syncSceneCamera();
    }
}

Entity editor::SceneRender::getPreviewCameraEntity() const{
    return previewCameraEntity;
}

bool editor::SceneRender::isPreviewCameraActive(){
    // Self-healing query: if the previewed camera entity was destroyed or lost its
    // required components, clear it here so callers (and the UI) fall back to the
    // editor camera on the same frame.
    if (isPreviewCameraUsable(previewCameraEntity)){
        return true;
    }

    previewCameraEntity = NULL_ENTITY;
    return false;
}

void editor::SceneRender::beginGameViewPass(Entity cameraEntity, Framebuffer* target){
    gameViewCameraEntity = cameraEntity;
    hideAllGizmos();
    syncSceneCamera();

    Engine::setFramebuffer(target);
    Engine::setScene(scene);
    scene->getSystem<RenderSystem>()->setDrawEditorOverlays(false);
    // the Game view culls as the game does, whatever the Scene view shows
    scene->getSystem<RenderSystem>()->setDisableFaceCulling(false);
    // and draws the scene's post-processing
    scene->getSystem<RenderSystem>()->setDisablePostProcessing(false);

    // Only the game's own layers: no gizmo, overlay or view-gizmo scenes
    Engine::removeAllSceneLayers(false);
    engineLayersActive = false;
    engineLayerTools = -1;
    for (Scene* childScene : childSceneLayers) {
        // the game's layers draw as the game draws them: without the editor overlays living
        // in them (a frame drawing them is a Scene view frame: it also skips HTML UI documents)
        childScene->getSystem<RenderSystem>()->setDrawEditorOverlays(false);
        Engine::addSceneLayer(childScene);
    }
}

void editor::SceneRender::endGameViewPass(){
    gameViewCameraEntity = NULL_ENTITY;
    scene->getSystem<RenderSystem>()->setDisablePostProcessing(viewDisablesPostProcessing);
    if (!isPlaying){
        syncSceneCamera();
    }
}

Scene* editor::SceneRender::gizmoScene() const{
    return editLayer.active ? editLayer.scene : scene;
}

int editor::SceneRender::layerToolsIndex() const{
    return editLayer.view.isOrthographic() ? 0 : 1;
}

editor::ToolsLayer& editor::SceneRender::getLayerTools(int index){
    if (!layerTools[index]){
        layerTools[index] = std::make_unique<ToolsLayer>(index == 0);
        ScopedDefaultEntityPool sys(*layerTools[index]->getScene(), EntityPool::System);
        layerSelLines[index] = new LineRenderer(layerTools[index]->getScene());
        layerSelLines[index]->addLine(Vector3::ZERO, Vector3::ZERO, Vector4::ZERO);
        layerSelLines[index]->setVisible(false);
        layerTools[index]->setGizmoVisible(false);
        // drawn through Engine::drawView before (or without) the Engine ever adding it
        if (Engine::isViewLoaded()){
            layerTools[index]->getScene()->load();
        }
    }
    return *layerTools[index];
}

editor::ToolsLayer& editor::SceneRender::gizmoTools(){
    return editLayer.active ? *layerTools[layerToolsIndex()] : toolslayer;
}

const editor::ToolsLayer& editor::SceneRender::gizmoTools() const{
    return editLayer.active ? *layerTools[layerToolsIndex()] : toolslayer;
}

editor::ToolsLayer* editor::SceneRender::getActiveToolsLayer(){
    return &gizmoTools();
}

Ray editor::SceneRender::gizmoRay(float x, float y){
    return editLayer.active ? editLayer.view.ray(x, y) : screenToRay(x, y);
}

Vector3 editor::SceneRender::gizmoViewDirection() const{
    return editLayer.active ? editLayer.view.worldDirection : camera->getWorldDirection();
}

bool editor::SceneRender::isOrthographicView(Scene* target) const{
    if (editLayer.active && target == editLayer.scene && target != scene){
        return editLayer.view.isOrthographic();
    }
    return scene->getComponent<CameraComponent>(camera->getEntity()).type == CameraType::CAMERA_ORTHO;
}

float editor::SceneRender::targetBillboardScale(Scene* target, const Vector3& worldPos, float logicalScale) const{
    if (editLayer.active && target == editLayer.scene && target != scene){
        return overlayPx(logicalScale) * editLayer.view.worldPerPixel(worldPos);
    }
    return billboardScreenScale(worldPos, logicalScale);
}

// The outline of each box: its near face (2D views), or all twelve edges
void editor::SceneRender::fillSelectionLines(LineRenderer* lines, const std::vector<OBB>& obbs, bool boxes){
    const auto& edges = BOX_EDGES;
    const Vector4 color = Vector4(1.0, 0.6, 0.0, 1.0);
    const size_t edgesPerBox = boxes ? 12 : 4;

    if (lines->getNumLines() != obbs.size() * edgesPerBox){
        lines->clearLines();
        for (const OBB& obb : obbs){
            for (size_t e = 0; e < edgesPerBox; e++){
                lines->addLine(obb.getCorner(edges[e][0]), obb.getCorner(edges[e][1]), color);
            }
        }
    }else{
        size_t line = 0;
        for (const OBB& obb : obbs){
            for (size_t e = 0; e < edgesPerBox; e++){
                lines->updateLine(line++, obb.getCorner(edges[e][0]), obb.getCorner(edges[e][1]));
            }
        }
    }
}

// The Engine draws this scene's editor layers while activate() set them: the edit
// layer's tools join them, or leave, as it comes and goes
void editor::SceneRender::syncEngineLayerTools(){
    const int wanted = editLayer.active ? layerToolsIndex() : -1;
    if (wanted == engineLayerTools){
        return;
    }
    if (!engineLayersActive || Engine::getScene() != scene){
        // the Engine draws another view now; activate() adds them when it is this one again
        engineLayerTools = -1;
        engineLayersActive = false;
        return;
    }
    if (engineLayerTools >= 0 && layerTools[engineLayerTools]){
        Engine::removeScene(layerTools[engineLayerTools]->getScene());
    }
    if (wanted >= 0){
        Engine::addSceneLayer(layerTools[wanted]->getScene());
    }
    engineLayerTools = wanted;
}

void editor::SceneRender::setEditLayer(uint32_t sceneId, Scene* layerScene, const LayerView& view, const std::vector<Entity>& selection,
                                       const std::vector<Entity>& entities, bool running){
    if (!layerScene || layerScene == scene){
        clearEditLayer();
        return;
    }

    const bool wasActive = editLayer.active;
    const int previousTools = wasActive ? layerToolsIndex() : -1;

    if (!wasActive){
        // the sub-selections (tile, instance, points) are of this scene's GameObjects
        clearTileSelection();
        clearInstanceSelection();
        clearOccluderPointSelection();
        clearLinePointSelection();
        clearPolygonPointSelection();
        clearJointAnchorSelection();
        clearCapsuleHandleSelection();
        clearTrackPointSelection();
    }
    if (!wasActive || editLayer.sceneId != sceneId){
        // a drag that began on another target does not carry over
        mouseClicked = false;
    }

    editLayer.active = true;
    editLayer.sceneId = sceneId;
    editLayer.scene = layerScene;
    editLayer.view = view;
    editLayer.selection = selection;
    editLayer.entities = entities;
    editLayer.running = running;

    ToolsLayer& tools = getLayerTools(layerToolsIndex());
    tools.setGizmoSelected(toolslayer.getGizmoSelected());
    tools.updateCamera(view);

    if (previousTools >= 0 && previousTools != layerToolsIndex()){
        layerTools[previousTools]->setGizmoVisible(false);
        layerSelLines[previousTools]->setVisible(false);
    }
    if (!wasActive){
        // this scene's own gizmo stays hidden until the tools come back to it
        toolslayer.setGizmoVisible(false);
        selLines->setVisible(false);
    }

    syncEngineLayerTools();
}

void editor::SceneRender::clearEditLayer(){
    if (!editLayer.active){
        return;
    }

    for (int i = 0; i < 2; i++){
        if (layerTools[i]){
            layerTools[i]->mouseRelease();
            layerTools[i]->setGizmoVisible(false);
            layerSelLines[i]->setVisible(false);
        }
    }
    editLayer = EditLayer();
    mouseClicked = false;

    syncEngineLayerTools();
}

bool editor::SceneRender::hasEditLayer() const{
    return editLayer.active;
}

uint32_t editor::SceneRender::getEditLayerSceneId() const{
    return editLayer.active ? editLayer.sceneId : 0;
}

bool editor::SceneRender::isEditLayerYDown() const{
    return editLayer.active && editLayer.view.type == CameraType::CAMERA_UI;
}

Rect editor::SceneRender::getGameLayerRect(int width, int height) const{
    const Rect full(0, 0, (float)width, (float)height);
    const float canvasWidth = (float)Engine::getCanvasWidth();
    const float canvasHeight = (float)Engine::getCanvasHeight();
    if (canvasWidth <= 0.0f || canvasHeight <= 0.0f || width <= 0 || height <= 0){
        return full;
    }

    const float fit = std::min((float)width / canvasWidth, (float)height / canvasHeight);
    float rectWidth = std::max(1.0f, std::floor(canvasWidth * fit));
    float rectHeight = std::max(1.0f, std::floor(canvasHeight * fit));
    // whole margins on both sides: the rect stays exactly centered, which a layer
    // camera's whole-view equivalent (LayerView) needs for a perspective camera
    if ((width - (int)rectWidth) % 2 != 0 && rectWidth > 1.0f){
        rectWidth -= 1.0f;
    }
    if ((height - (int)rectHeight) % 2 != 0 && rectHeight > 1.0f){
        rectHeight -= 1.0f;
    }
    return Rect(((float)width - rectWidth) * 0.5f, ((float)height - rectHeight) * 0.5f, rectWidth, rectHeight);
}

editor::ToolsLayer* editor::SceneRender::getToolsLayer(){
    return &toolslayer;
}

editor::UILayer* editor::SceneRender::getUILayer(){
    return &uilayer;
}

bool editor::SceneRender::isUseGlobalTransform() const{
    return useGlobalTransform;
}

void editor::SceneRender::setUseGlobalTransform(bool useGlobalTransform){
    this->useGlobalTransform = useGlobalTransform;
}

void editor::SceneRender::enableCursorPointer(){
    cursorSelected = CursorSelected::POINTER;
}

void editor::SceneRender::enableCursorHand(){
    cursorSelected = CursorSelected::HAND;
}

editor::CursorSelected editor::SceneRender::getCursorSelected() const{
    return cursorSelected;
}

bool editor::SceneRender::isMultipleEntitesSelected() const{
    return multipleEntitiesSelected;
}

void editor::SceneRender::selectTile(Entity entity, int tileIndex){
    selectedTileEntity = entity;
    selectedTileIndex = tileIndex;
}

void editor::SceneRender::clearTileSelection(){
    selectedTileEntity = 0;
    selectedTileIndex = -1;
}

void editor::SceneRender::selectOccluderPoint(Entity entity, int pointIndex){
    selectedOccluderPointEntity = entity;
    selectedOccluderPointIndex = pointIndex;
}

void editor::SceneRender::clearOccluderPointSelection(){
    selectedOccluderPointEntity = 0;
    selectedOccluderPointIndex = -1;
}

// Returns the polygon point under the mouse (-1 when none). Only POLYGON
// occluders have editable points.
int editor::SceneRender::hitTestOccluderPoint(Entity entity, float x, float y){
    if (!scene->getComponentArray<ShadowCaster2DComponent>()->hasEntity(entity)) return -1;
    if (!scene->getComponentArray<Transform>()->hasEntity(entity)) return -1;

    ShadowCaster2DComponent& occluder = scene->getComponent<ShadowCaster2DComponent>(entity);
    Transform& transform = scene->getComponent<Transform>(entity);

    if (occluder.shape != ShadowCaster2DShape::POLYGON) return -1;

    Ray ray = screenToRay(x, y);
    RayReturn rr = ray.intersects(Plane(Vector3(0, 0, 1), transform.worldPosition));
    if (!rr) return -1;

    // handles are drawn at overlayPx(4)*zoom half-extent; use a slightly larger grab radius
    float hitRadius = overlayPx(8.0f) * zoom;
    int bestIndex = -1;
    float bestDistance = hitRadius;
    for (size_t i = 0; i < occluder.points.size(); i++){
        Vector3 worldPoint = transform.modelMatrix * Vector3(occluder.points[i].x, occluder.points[i].y, 0.0f);
        float dist = Vector2(worldPoint.x - rr.point.x, worldPoint.y - rr.point.y).length();
        if (dist <= bestDistance){
            bestDistance = dist;
            bestIndex = (int)i;
        }
    }

    return bestIndex;
}

OBB editor::SceneRender::getOccluderPointOBB(Entity entity, int pointIndex){
    if (!scene->getComponentArray<ShadowCaster2DComponent>()->hasEntity(entity)) return OBB();
    if (!scene->getComponentArray<Transform>()->hasEntity(entity)) return OBB();

    ShadowCaster2DComponent& occluder = scene->getComponent<ShadowCaster2DComponent>(entity);
    Transform& transform = scene->getComponent<Transform>(entity);

    if (occluder.shape != ShadowCaster2DShape::POLYGON) return OBB();
    if (pointIndex < 0 || pointIndex >= (int)occluder.points.size()) return OBB();

    Vector3 worldPoint = transform.modelMatrix * Vector3(occluder.points[pointIndex].x, occluder.points[pointIndex].y, 0.0f);
    float halfSize = overlayPx(8.0f) * zoom;
    AABB aabb(worldPoint - Vector3(halfSize, halfSize, 1.0f), worldPoint + Vector3(halfSize, halfSize, 1.0f));
    return aabb.getOBB();
}

void editor::SceneRender::selectLinePoint(Entity entity, int pointIndex){
    selectedLinePointEntity = entity;
    selectedLinePointIndex = pointIndex;
    const LineRendererComponent* lines = scene->findComponent<LineRendererComponent>(entity);
    selectedLinePointMode = lines ? lines->mode : LineRendererMode::Segments;
}

void editor::SceneRender::clearLinePointSelection(){
    selectedLinePointEntity = 0;
    selectedLinePointIndex = -1;
}

float editor::SceneRender::getPointHandleHalfSize(const Vector3& worldPoint){
    return billboardScreenScale(worldPoint, 8.0f);
}

std::vector<Vector3> editor::SceneRender::getLinePointWorldPositions(const LineRendererComponent& lines, const Transform& transform){
    std::vector<Vector3> points;
    if (lines.mode == LineRendererMode::Polyline){
        points.reserve(lines.positions.size());
        for (const Vector3& position : lines.positions){
            points.push_back(lines.useWorldSpace ? position : transform.modelMatrix * position);
        }
    }else{
        points.reserve(lines.lines.size() * 2);
        for (const LineData& line : lines.lines){
            points.push_back(lines.useWorldSpace ? line.pointA : transform.modelMatrix * line.pointA);
            points.push_back(lines.useWorldSpace ? line.pointB : transform.modelMatrix * line.pointB);
        }
    }
    return points;
}

std::string editor::SceneRender::getLinePointProperty(const LineRendererComponent& lines, int pointIndex){
    if (lines.mode == LineRendererMode::Polyline){
        return "positions[" + std::to_string(pointIndex) + "]";
    }
    return "lines[" + std::to_string(pointIndex / 2) + ((pointIndex % 2 == 0) ? "].pointA" : "].pointB");
}

Vector3 editor::SceneRender::toLinePointSpace(const LineRendererComponent& lines, const Transform& transform, const Vector3& world){
    return lines.useWorldSpace ? world : transform.modelMatrix.inverse() * world;
}

void editor::SceneRender::mergeLineLocalPoints(AABB& aabb, const LineRendererComponent& lines, const Transform& transform){
    const Matrix4 toLocal = lines.useWorldSpace ? transform.modelMatrix.inverse() : Matrix4();
    auto merge = [&](const Vector3& point){
        aabb.merge(lines.useWorldSpace ? toLocal * point : point);
    };
    if (lines.mode == LineRendererMode::Polyline){
        for (const Vector3& position : lines.positions) merge(position);
    }else{
        for (const LineData& line : lines.lines){
            merge(line.pointA);
            merge(line.pointB);
        }
    }
}

// Returns the line point under the mouse (-1 when none): a polyline's position index, or for
// segments lineIndex * 2 + endpoint (0 = pointA, 1 = pointB).
int editor::SceneRender::hitTestLinePoint(Entity entity, float x, float y){
    if (!scene->getComponentArray<LineRendererComponent>()->hasEntity(entity)) return -1;
    if (!scene->getComponentArray<Transform>()->hasEntity(entity)) return -1;

    LineRendererComponent& lines = scene->getComponent<LineRendererComponent>(entity);
    Transform& transform = scene->getComponent<Transform>(entity);

    Ray ray = screenToRay(x, y);

    // ray test against each point's handle box (works in both ortho and perspective views),
    // keeping the nearest hit like instance picking
    int bestIndex = -1;
    float bestDistance = FLT_MAX;
    const std::vector<Vector3> points = getLinePointWorldPositions(lines, transform);
    for (size_t i = 0; i < points.size(); i++){
        const Vector3& worldPoint = points[i];
        float halfSize = getPointHandleHalfSize(worldPoint);
        AABB handleAABB(worldPoint - Vector3(halfSize), worldPoint + Vector3(halfSize));

        RayReturn rr = ray.intersects(handleAABB);
        if (rr && rr.distance < bestDistance){
            bestDistance = rr.distance;
            bestIndex = (int)i;
        }
    }

    return bestIndex;
}

OBB editor::SceneRender::getLinePointOBB(Entity entity, int pointIndex){
    if (!scene->getComponentArray<LineRendererComponent>()->hasEntity(entity)) return OBB();
    if (!scene->getComponentArray<Transform>()->hasEntity(entity)) return OBB();

    LineRendererComponent& lines = scene->getComponent<LineRendererComponent>(entity);
    Transform& transform = scene->getComponent<Transform>(entity);

    const std::vector<Vector3> points = getLinePointWorldPositions(lines, transform);
    if (pointIndex < 0 || pointIndex >= (int)points.size()) return OBB();

    const Vector3& worldPoint = points[pointIndex];
    float halfSize = getPointHandleHalfSize(worldPoint);
    AABB aabb(worldPoint - Vector3(halfSize), worldPoint + Vector3(halfSize));
    return aabb.getOBB();
}

int editor::SceneRender::insertLinePointAt(Project* project, uint32_t commandSceneId, Entity entity, float x, float y){
    LineRendererComponent* lines = scene->findComponent<LineRendererComponent>(entity);
    Transform* transform = scene->findComponent<Transform>(entity);
    if (!lines || !transform || lines->mode != LineRendererMode::Polyline || lines->positions.size() < 2) return -1;

    // the segment nearest the cursor on screen, within a few pixels of it (or of its width)
    const std::vector<Vector3> points = getLinePointWorldPositions(*lines, *transform);
    const size_t segments = lines->loop ? points.size() : points.size() - 1;
    const Vector2 cursor(x, y);
    float bestDistance = overlayPx(10.0f);
    int bestSegment = -1;
    float bestT = 0.0f;
    for (size_t s = 0; s < segments; s++){
        Vector2 a, b;
        if (!worldToScreen(points[s], a) || !worldToScreen(points[(s + 1) % points.size()], b)) continue;
        const Vector2 ab = b - a;
        const float lengthSquared = ab.dotProduct(ab);
        const float t = lengthSquared > 0.0f ? std::min(1.0f, std::max(0.0f, (cursor - a).dotProduct(ab) / lengthSquared)) : 0.0f;
        const float distance = (a + ab * t - cursor).length();
        if (distance < bestDistance){
            bestDistance = distance;
            bestSegment = (int)s;
            bestT = t;
        }
    }
    if (bestSegment < 0) return -1;

    // a point between the segment's two, in the positions' own space
    const Vector3& a = lines->positions[(size_t)bestSegment];
    const Vector3& b = lines->positions[((size_t)bestSegment + 1) % lines->positions.size()];
    std::vector<Vector3> positions = lines->positions;
    const int index = bestSegment + 1;
    positions.insert(positions.begin() + index, a + (b - a) * bestT);
    CommandHandle::get(commandSceneId)->addCommandNoMerge(
        new PropertyCmd<std::vector<Vector3>>(project, commandSceneId, entity, ComponentType::LineRendererComponent, "positions", positions));
    selectLinePoint(entity, index);
    return index;
}

bool editor::SceneRender::removeSelectedLinePoint(Project* project, uint32_t commandSceneId){
    if (selectedLinePointIndex < 0) return false;
    // only the point of a selected GameObject's polyline, picked in that mode
    const std::vector<Entity> selected = project->getSelectedEntities(commandSceneId);
    LineRendererComponent* lines = scene->findComponent<LineRendererComponent>(selectedLinePointEntity);
    if (!lines || lines->mode != LineRendererMode::Polyline || selectedLinePointMode != LineRendererMode::Polyline ||
            selectedLinePointIndex >= (int)lines->positions.size() ||
            std::find(selected.begin(), selected.end(), selectedLinePointEntity) == selected.end()){
        clearLinePointSelection();
        return false;
    }
    std::vector<Vector3> positions = lines->positions;
    positions.erase(positions.begin() + selectedLinePointIndex);
    CommandHandle::get(commandSceneId)->addCommandNoMerge(
        new PropertyCmd<std::vector<Vector3>>(project, commandSceneId, selectedLinePointEntity, ComponentType::LineRendererComponent, "positions", positions));
    clearLinePointSelection();
    return true;
}

void editor::SceneRender::selectPolygonPoint(Entity entity, bool isMesh, int pointIndex){
    selectedPolygonPointEntity = entity;
    selectedPolygonPointIsMesh = isMesh;
    selectedPolygonPointIndex = pointIndex;
}

void editor::SceneRender::clearPolygonPointSelection(){
    selectedPolygonPointEntity = 0;
    selectedPolygonPointIndex = -1;
}

// Polygon and MeshPolygon both store std::vector<PolygonPoint> (Vector3 position);
// isMesh chooses which component so one code path serves both.
bool editor::SceneRender::getPolygonPointWorld(Entity entity, bool isMesh, int pointIndex, Vector3& worldPoint){
    if (!scene->getComponentArray<Transform>()->hasEntity(entity)) return false;
    Transform& transform = scene->getComponent<Transform>(entity);

    if (isMesh){
        if (!scene->getComponentArray<MeshPolygonComponent>()->hasEntity(entity)) return false;
        MeshPolygonComponent& polygon = scene->getComponent<MeshPolygonComponent>(entity);
        if (pointIndex < 0 || pointIndex >= (int)polygon.points.size()) return false;
        worldPoint = transform.modelMatrix * polygon.points[pointIndex].position;
    }else{
        if (!scene->getComponentArray<PolygonComponent>()->hasEntity(entity)) return false;
        PolygonComponent& polygon = scene->getComponent<PolygonComponent>(entity);
        if (pointIndex < 0 || pointIndex >= (int)polygon.points.size()) return false;
        worldPoint = transform.modelMatrix * polygon.points[pointIndex].position;
    }

    return true;
}

// Returns the polygon vertex under the mouse (-1 when none). Ray-vs-handle-box test
// so it works in both ortho and perspective views (like line endpoints).
int editor::SceneRender::hitTestPolygonPoint(Entity entity, bool isMesh, float x, float y){
    size_t count = 0;
    if (isMesh){
        if (!scene->getComponentArray<MeshPolygonComponent>()->hasEntity(entity)) return -1;
        count = scene->getComponent<MeshPolygonComponent>(entity).points.size();
    }else{
        if (!scene->getComponentArray<PolygonComponent>()->hasEntity(entity)) return -1;
        count = scene->getComponent<PolygonComponent>(entity).points.size();
    }

    Ray ray = screenToRay(x, y);

    int bestIndex = -1;
    float bestDistance = FLT_MAX;
    for (size_t i = 0; i < count; i++){
        Vector3 worldPoint;
        if (!getPolygonPointWorld(entity, isMesh, (int)i, worldPoint)) continue;
        float halfSize = getPointHandleHalfSize(worldPoint);
        AABB handleAABB(worldPoint - Vector3(halfSize), worldPoint + Vector3(halfSize));

        RayReturn rr = ray.intersects(handleAABB);
        if (rr && rr.distance < bestDistance){
            bestDistance = rr.distance;
            bestIndex = (int)i;
        }
    }

    return bestIndex;
}

OBB editor::SceneRender::getPolygonPointOBB(Entity entity, bool isMesh, int pointIndex){
    Vector3 worldPoint;
    if (!getPolygonPointWorld(entity, isMesh, pointIndex, worldPoint)) return OBB();

    float halfSize = getPointHandleHalfSize(worldPoint);
    AABB aabb(worldPoint - Vector3(halfSize), worldPoint + Vector3(halfSize));
    return aabb.getOBB();
}

void editor::SceneRender::selectJointAnchor(Entity entity, int index){
    selectedJointAnchorEntity = entity;
    selectedJointAnchorIndex = index;
}

void editor::SceneRender::clearJointAnchorSelection(){
    selectedJointAnchorEntity = 0;
    selectedJointAnchorIndex = -1;
}

std::string editor::SceneRender::getJointAnchorProperty(Entity entity, int index){
    Joint2DComponent* joint = scene->findComponent<Joint2DComponent>(entity);
    if (!joint) return std::string();
    const bool usesAnchors = joint->type != Joint2DType::MOUSE && joint->type != Joint2DType::MOTOR;
    if (index == 0) return usesAnchors ? "anchorA" : std::string();
    if (index == 1){
        if (joint->type == Joint2DType::MOUSE) return "target";
        if (joint2DAnchorsWorld(scene, *joint).anchorBEditable) return "anchorB";
    }
    return std::string();
}

bool editor::SceneRender::getJointAnchorWorld(Entity entity, int index, Vector3& worldPoint){
    Joint2DComponent* joint = scene->findComponent<Joint2DComponent>(entity);
    if (!joint || getJointAnchorProperty(entity, index).empty()) return false;
    const Joint2DAnchorsWorld anchors = joint2DAnchorsWorld(scene, *joint);
    worldPoint = index == 0 ? anchors.anchorA : anchors.anchorB;
    return true;
}

// the anchor handle under the mouse (-1 when none)
int editor::SceneRender::hitTestJointAnchor(Entity entity, float x, float y){
    Ray ray = screenToRay(x, y);

    int bestIndex = -1;
    float bestDistance = FLT_MAX;
    for (int i = 0; i < 2; i++){
        Vector3 worldPoint;
        if (!getJointAnchorWorld(entity, i, worldPoint)) continue;
        float halfSize = getPointHandleHalfSize(worldPoint);
        AABB handleAABB(worldPoint - Vector3(halfSize), worldPoint + Vector3(halfSize));

        RayReturn rr = ray.intersects(handleAABB);
        if (rr && rr.distance < bestDistance){
            bestDistance = rr.distance;
            bestIndex = i;
        }
    }

    return bestIndex;
}

OBB editor::SceneRender::getJointAnchorOBB(Entity entity, int index){
    Vector3 worldPoint;
    if (!getJointAnchorWorld(entity, index, worldPoint)) return OBB();

    float halfSize = getPointHandleHalfSize(worldPoint);
    AABB aabb(worldPoint - Vector3(halfSize), worldPoint + Vector3(halfSize));
    return aabb.getOBB();
}

void editor::SceneRender::selectCapsuleHandle(Entity entity, int index){
    selectedCapsuleHandleEntity = entity;
    selectedCapsuleHandleIndex = index;
}

void editor::SceneRender::clearCapsuleHandleSelection(){
    selectedCapsuleHandleEntity = 0;
    selectedCapsuleHandleIndex = -1;
    capsuleDragBase.clear();
}

Entity editor::SceneRender::findCapsuleBone(Entity entity, const std::string& bone){
    if (!scene->findComponent<Transform>(entity)) return NULL_ENTITY;
    if (bone.empty()) return entity;
    // the GameObject's branch is contiguous in the Transforms (hierarchy order)
    auto transforms = scene->getComponentArray<Transform>();
    const size_t first = transforms->getIndex(entity);
    const size_t last = scene->findBranchLastIndex(entity);
    for (size_t i = first + 1; i <= last && i < transforms->size(); i++){
        const Entity candidate = transforms->getEntity(i);
        if (scene->getEntityName(candidate) == bone) return candidate;
    }
    return NULL_ENTITY;
}

namespace {

    // the radius handle's direction from the middle of a capsule: across its segment, level when
    // the segment is not vertical
    Vector3 capsuleRadiusDirection(const CapsuleShadows::World& capsule){
        Vector3 axis = capsule.b - capsule.a;
        if (axis.squaredLength() < 1e-12f) axis = Vector3(0.0f, 1.0f, 0.0f);
        axis.normalize();
        Vector3 across = axis.crossProduct(Vector3(0.0f, 1.0f, 0.0f));
        if (across.squaredLength() < 1e-6f) across = axis.crossProduct(Vector3(1.0f, 0.0f, 0.0f));
        return across.normalized();
    }

    // the world size of one unit of a shape's radius on a bone (the scale toWorld gives the radius)
    float capsuleRadiusScale(const CapsuleShadowShape& shape, const Matrix4& boneModel){
        CapsuleShadowShape unit = shape;
        unit.radius = 1.0f;
        return CapsuleShadows::toWorld(unit, boneModel).radius;
    }

}

bool editor::SceneRender::getCapsuleWorld(Entity entity, size_t capsule, CapsuleShadows::World& world){
    const CapsuleShadowCasterComponent* caster = scene->findComponent<CapsuleShadowCasterComponent>(entity);
    if (!caster || capsule >= caster->capsules.size()) return false;
    const Entity bone = findCapsuleBone(entity, caster->capsules[capsule].bone);
    const Transform* boneTransform = bone != NULL_ENTITY ? scene->findComponent<Transform>(bone) : nullptr;
    if (!boneTransform) return false;
    world = CapsuleShadows::toWorld(caster->capsules[capsule], boneTransform->modelMatrix);
    return true;
}

bool editor::SceneRender::getCapsuleHandleWorld(Entity entity, int index, Vector3& worldPoint){
    if (index < 0) return false;
    CapsuleShadows::World world;
    if (!getCapsuleWorld(entity, (size_t)(index / CAPSULE_HANDLES), world)) return false;
    const int handle = index % CAPSULE_HANDLES;
    if (handle == 0){
        worldPoint = world.a;
    }else if (handle == 1){
        worldPoint = world.b;
    }else{
        worldPoint = (world.a + world.b) * 0.5f + capsuleRadiusDirection(world) * world.radius;
    }
    return true;
}

// the capsule handle under the mouse (-1 when none)
int editor::SceneRender::hitTestCapsuleHandle(Entity entity, float x, float y){
    const CapsuleShadowCasterComponent* caster = scene->findComponent<CapsuleShadowCasterComponent>(entity);
    if (!caster) return -1;
    Ray ray = screenToRay(x, y);

    int bestIndex = -1;
    float bestDistance = FLT_MAX;
    const int count = (int)caster->capsules.size() * CAPSULE_HANDLES;
    for (int i = 0; i < count; i++){
        Vector3 worldPoint;
        if (!getCapsuleHandleWorld(entity, i, worldPoint)) continue;
        float halfSize = getPointHandleHalfSize(worldPoint);
        AABB handleAABB(worldPoint - Vector3(halfSize), worldPoint + Vector3(halfSize));

        RayReturn rr = ray.intersects(handleAABB);
        if (rr && rr.distance < bestDistance){
            bestDistance = rr.distance;
            bestIndex = i;
        }
    }

    return bestIndex;
}

OBB editor::SceneRender::getCapsuleHandleOBB(Entity entity, int index){
    Vector3 worldPoint;
    if (!getCapsuleHandleWorld(entity, index, worldPoint)) return OBB();

    float halfSize = getPointHandleHalfSize(worldPoint);
    AABB aabb(worldPoint - Vector3(halfSize), worldPoint + Vector3(halfSize));
    return aabb.getOBB();
}

bool editor::SceneRender::capsulesWithHandleAt(Entity entity, int index, const Vector3& world, const std::vector<CapsuleShadowShape>& base,
                                               std::vector<CapsuleShadowShape>& capsules){
    if (index < 0) return false;
    const size_t capsule = (size_t)(index / CAPSULE_HANDLES);
    if (capsule >= base.size()) return false;
    const CapsuleShadowShape& shape = base[capsule];
    const Entity bone = findCapsuleBone(entity, shape.bone);
    const Transform* boneTransform = bone != NULL_ENTITY ? scene->findComponent<Transform>(bone) : nullptr;
    if (!boneTransform) return false;

    const CapsuleShadows::World start = CapsuleShadows::toWorld(shape, boneTransform->modelMatrix);
    capsules = base;
    const int handle = index % CAPSULE_HANDLES;
    if (handle == 2){
        // the distance from the segment, in the bone's units
        const Vector3 axis = start.b - start.a;
        const float lengthSq = axis.squaredLength();
        const float t = lengthSq > 0.0f ? std::clamp((world - start.a).dotProduct(axis) / lengthSq, 0.0f, 1.0f) : 0.0f;
        const float worldRadius = (world - (start.a + axis * t)).length();
        const float scale = capsuleRadiusScale(shape, boneTransform->modelMatrix);
        capsules[capsule].radius = scale > 0.0f ? worldRadius / scale : worldRadius;
        return true;
    }

    CapsuleShadows::Bone boneSpace;
    boneSpace.position = boneTransform->worldPosition;
    boneSpace.rotation = boneTransform->worldRotation;
    boneSpace.scale = boneTransform->worldScale;
    const Vector3 a = handle == 0 ? world : start.a;
    const Vector3 b = handle == 1 ? world : start.b;
    capsules[capsule] = CapsuleShadows::fromWorld(shape.bone, boneSpace, a, b, start.radius);
    return true;
}

void editor::SceneRender::selectTrackPoint(Entity entity, int pointIndex){
    selectedTrackPointEntity = entity;
    selectedTrackPointIndex = pointIndex;
}

void editor::SceneRender::clearTrackPointSelection(){
    selectedTrackPointEntity = 0;
    selectedTrackPointIndex = -1;
}

Matrix4 editor::SceneRender::getTrackPointsWorldMatrix(Entity entity){
    // values[] drive the action target's Transform::position, which is local to
    // the target's parent: that parent's model matrix maps them to world space
    ActionComponent* action = scene->findComponent<ActionComponent>(entity);
    if (!action || action->target == NULL_ENTITY || !scene->isEntityCreated(action->target)){
        return Matrix4();
    }
    Transform* targetTransform = scene->findComponent<Transform>(action->target);
    if (!targetTransform){
        return Matrix4();
    }

    Matrix4 matrix;
    if (targetTransform->parent != NULL_ENTITY){
        if (Transform* parentTransform = scene->findComponent<Transform>(targetTransform->parent)){
            matrix = parentTransform->modelMatrix;
        }
    }

    // relative values are offsets from where the track started, or from the target before it starts
    KeyframeTracksComponent* keyframe = scene->findComponent<KeyframeTracksComponent>(entity);
    if (keyframe && keyframe->relative){
        ActionSystem* actions = scene->getSystem<ActionSystem>().get();
        Vector3 base = keyframe->basePosition;
        if (!keyframe->hasBase){
            Vector3 scale;
            Quaternion rotation;
            actions->getRelativeTrackPose(action->target, base, rotation, scale);
        }
        matrix = matrix * actions->getRelativeTrackMatrix(action->target, base);
    }

    return matrix;
}

bool editor::SceneRender::getTrackPointWorld(Entity entity, int pointIndex, Vector3& worldPoint){
    TranslateTracksComponent* tracks = scene->findComponent<TranslateTracksComponent>(entity);
    if (!tracks) return false;
    if (pointIndex < 0 || pointIndex >= (int)tracks->values.size()) return false;

    worldPoint = getTrackPointsWorldMatrix(entity) * tracks->values[pointIndex];
    return true;
}

int editor::SceneRender::hitTestTrackPoint(Entity entity, float x, float y){
    TranslateTracksComponent* tracks = scene->findComponent<TranslateTracksComponent>(entity);
    if (!tracks) return -1;

    Ray ray = screenToRay(x, y);
    Matrix4 worldMatrix = getTrackPointsWorldMatrix(entity);

    int bestIndex = -1;
    float bestDistance = FLT_MAX;
    for (size_t i = 0; i < tracks->values.size(); i++){
        Vector3 worldPoint = worldMatrix * tracks->values[i];
        float halfSize = getPointHandleHalfSize(worldPoint);
        AABB handleAABB(worldPoint - Vector3(halfSize), worldPoint + Vector3(halfSize));

        RayReturn rr = ray.intersects(handleAABB);
        if (rr && rr.distance < bestDistance){
            bestDistance = rr.distance;
            bestIndex = (int)i;
        }
    }

    return bestIndex;
}

OBB editor::SceneRender::getTrackPointOBB(Entity entity, int pointIndex){
    Vector3 worldPoint;
    if (!getTrackPointWorld(entity, pointIndex, worldPoint)) return OBB();

    float halfSize = getPointHandleHalfSize(worldPoint);
    AABB aabb(worldPoint - Vector3(halfSize), worldPoint + Vector3(halfSize));
    return aabb.getOBB();
}

int editor::SceneRender::hitTestTile(Entity entity, float x, float y){
    if (!scene->getComponentArray<TilemapComponent>()->hasEntity(entity)) return -1;
    if (!scene->getComponentArray<Transform>()->hasEntity(entity)) return -1;

    TilemapComponent& tilemap = scene->getComponent<TilemapComponent>(entity);
    Transform& transform = scene->getComponent<Transform>(entity);

    Ray ray = screenToRay(x, y);
    Plane tilePlane(Vector3(0, 0, 1), transform.worldPosition);
    RayReturn rr = ray.intersects(tilePlane);
    if (!rr) return -1;

    // Transform hit point to tilemap local space
    Matrix4 invModel = transform.modelMatrix.inverse();
    Vector3 localHit = invModel * rr.point;

    // Test tiles in reverse order (last drawn = on top)
    for (int i = (int)tilemap.numTiles - 1; i >= 0; i--){
        const TileData& tile = tilemap.tiles[i];
        if (localHit.x >= tile.position.x && localHit.x <= tile.position.x + tile.width &&
            localHit.y >= tile.position.y && localHit.y <= tile.position.y + tile.height){
            return i;
        }
    }

    return -1;
}

OBB editor::SceneRender::getTileOBB(Entity entity, int tileIndex, bool visual){
    if (!scene->getComponentArray<TilemapComponent>()->hasEntity(entity)) return OBB();
    if (!scene->getComponentArray<Transform>()->hasEntity(entity)) return OBB();

    TilemapComponent& tilemap = scene->getComponent<TilemapComponent>(entity);
    Transform& transform = scene->getComponent<Transform>(entity);

    if (tileIndex < 0 || tileIndex >= (int)tilemap.numTiles) return OBB();

    const TileData& tile = tilemap.tiles[tileIndex];
    AABB tileAABB(tile.position.x, tile.position.y, 0,
                  tile.position.x + tile.width, tile.position.y + tile.height, 0);

    return visual ? transformAABBPreservingShear(transform.modelMatrix, tileAABB)
                  : transform.modelMatrix * tileAABB.getOBB();
}

void editor::SceneRender::selectInstance(Entity entity, int instanceIndex){
    selectedInstanceEntity = entity;
    selectedInstanceIndex = instanceIndex;
}

void editor::SceneRender::clearInstanceSelection(){
    selectedInstanceEntity = 0;
    selectedInstanceIndex = -1;
}

Quaternion editor::SceneRender::getInstanceWorldRotation(const Transform& transform, const InstancedMeshComponent& instmesh, const InstanceData& inst) const{
    // Billboard mode overrides per-instance rotation at render time, so mirror that
    // here by not composing inst.rotation into the picking/gizmo orientation.
    if (instmesh.instancedBillboard){
        return transform.worldRotation;
    }
    return transform.worldRotation * inst.rotation;
}

OBB editor::SceneRender::getInstanceOBB(Entity entity, int instanceIndex, bool visual){
    if (!scene->getComponentArray<InstancedMeshComponent>()->hasEntity(entity)) return OBB();
    if (!scene->getComponentArray<MeshComponent>()->hasEntity(entity)) return OBB();
    if (!scene->getComponentArray<Transform>()->hasEntity(entity)) return OBB();

    InstancedMeshComponent& instmesh = scene->getComponent<InstancedMeshComponent>(entity);
    MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
    Transform& transform = scene->getComponent<Transform>(entity);

    if (instanceIndex < 0 || instanceIndex >= (int)instmesh.instances.size()) return OBB();

    const InstanceData& inst = instmesh.instances[instanceIndex];
    // For billboard meshes the per-instance rotation is ignored at render time, so
    // skip it here too to keep the OBB aligned with what is actually drawn.
    Matrix4 rotMatrix = instmesh.instancedBillboard
        ? Matrix4()
        : inst.rotation.getRotationMatrix();
    Matrix4 instanceMatrix = Matrix4::translateMatrix(inst.position)
                             * rotMatrix
                             * Matrix4::scaleMatrix(inst.scale);

    AABB localAABB = mesh.verticesAABB;
    if (localAABB.isNull() || (localAABB.getMinimum() == Vector3::ZERO && localAABB.getMaximum() == Vector3::ZERO)){
        // Fallback to a small unit cube around origin so the instance is still selectable
        localAABB = AABB(Vector3(-0.5f, -0.5f, -0.5f), Vector3(0.5f, 0.5f, 0.5f));
    }

    return visual ? transformAABBPreservingShear(transform.modelMatrix * instanceMatrix, localAABB)
                  : (transform.modelMatrix * instanceMatrix) * localAABB.getOBB();
}

int editor::SceneRender::hitTestInstance(Entity entity, float x, float y){
    if (!scene->getComponentArray<InstancedMeshComponent>()->hasEntity(entity)) return -1;
    if (!scene->getComponentArray<MeshComponent>()->hasEntity(entity)) return -1;
    if (!scene->getComponentArray<Transform>()->hasEntity(entity)) return -1;

    InstancedMeshComponent& instmesh = scene->getComponent<InstancedMeshComponent>(entity);

    Ray ray = screenToRay(x, y);

    // Only instances within maxInstances are actually rendered, so restrict picking to them.
    int renderedCount = (int)std::min<size_t>(instmesh.instances.size(), instmesh.maxInstances);

    int bestIndex = -1;
    float bestDistance = FLT_MAX;
    for (int i = 0; i < renderedCount; i++){
        if (!instmesh.instances[i].visible) continue;
        OBB instOBB = getInstanceOBB(entity, i);
        if (instOBB.isNull()) continue;
        RayReturn rr = ray.intersects(instOBB);
        if (rr && rr.distance < bestDistance){
            bestDistance = rr.distance;
            bestIndex = i;
        }
    }

    return bestIndex;
}

void editor::SceneRender::setupCameraIcon(CameraObjects& co){
    TextureData iconData;
    iconData.loadTextureFromMemory(camera_icon_png, camera_icon_png_len);
    co.icon->setTexture("editor:resources:camera_icon", iconData);
    co.icon->setSize(128, 128);
    co.icon->setReceiveLights(false);
    co.icon->setCastShadows(false);
    co.icon->setReceiveShadows(false);
    co.icon->setPivotPreset(PivotPreset::CENTER);
}

void editor::SceneRender::setupSoundIcon(SoundObjects& so){
    TextureData iconData;
    iconData.loadTextureFromMemory(audio_source_icon_png, audio_source_icon_png_len);
    so.icon->setTexture("editor:resources:sound_icon", iconData);
    so.icon->setSize(128, 128);
    so.icon->setReceiveLights(false);
    so.icon->setCastShadows(false);
    so.icon->setReceiveShadows(false);
    so.icon->setPivotPreset(PivotPreset::CENTER);
}

void editor::SceneRender::setupLight2DIcon(Light2DObjects& lo){
    TextureData iconData;
    iconData.loadTextureFromMemory(bulb_icon_png, bulb_icon_png_len);
    lo.icon->setTexture("editor:resources:bulb_icon", iconData);
    lo.icon->setSize(128, 128);
    lo.icon->setReceiveLights(false);
    lo.icon->setCastShadows(false);
    lo.icon->setReceiveShadows(false);
    lo.icon->setPivotPreset(PivotPreset::CENTER);
}

void editor::SceneRender::updateCameraFrustum(CameraObjects& co, const CameraComponent& cameraComponent, bool isMainCamera, bool fullFrustum){
    if (cameraComponent.type == CameraType::CAMERA_UI){
        co.lines->clearLines();
        co.type = CameraType::CAMERA_UI;
        return;
    }

    bool changed = false;
    if (co.type != cameraComponent.type) changed = true;
    if (co.isMainCamera != isMainCamera) changed = true;
    if (co.fullFrustum != fullFrustum) changed = true;
    if (cameraComponent.type == CameraType::CAMERA_PERSPECTIVE){
        if (co.yfov != cameraComponent.yfov || co.aspect != cameraComponent.aspect ||
            co.nearClip != cameraComponent.nearClip || co.farClip != cameraComponent.farClip){
            changed = true;
        }
    }else if (cameraComponent.type == CameraType::CAMERA_ORTHO){
        if (co.leftClip != cameraComponent.leftClip || co.rightClip != cameraComponent.rightClip ||
            co.bottomClip != cameraComponent.bottomClip || co.topClip != cameraComponent.topClip ||
            co.nearClip != cameraComponent.nearClip || co.farClip != cameraComponent.farClip){
            changed = true;
        }
    }

    if (!changed) return;

    co.type = cameraComponent.type;
    co.isMainCamera = isMainCamera;
    co.fullFrustum = fullFrustum;
    co.yfov = cameraComponent.yfov;
    co.aspect = cameraComponent.aspect;
    co.nearClip = cameraComponent.nearClip;
    co.farClip = cameraComponent.farClip;
    co.leftClip = cameraComponent.leftClip;
    co.rightClip = cameraComponent.rightClip;
    co.bottomClip = cameraComponent.bottomClip;
    co.topClip = cameraComponent.topClip;

    drawCameraFrustumLines(co.lines, cameraComponent, isMainCamera, fullFrustum);
}

std::array<Vector3, 8> editor::cameraGizmoCorners(const CameraComponent& camera, bool fullFrustum){
    // near and far as rendered (a perspective near <= 0 draws with the clamped one)
    const Vector2 nearFar = RenderSystem::getCameraNearFar(camera);
    float nearClip = nearFar.x;
    float farClip = nearFar.y;
    if (nearClip > farClip)
        std::swap(nearClip, farClip);

    if (!fullFrustum){
        // a compact gizmo for an unselected camera: a perspective one 2 units deep, an
        // orthographic one as deep as half its view is wide, so a wide view does not draw flat
        float depth = 2.0f;
        if (camera.type == CameraType::CAMERA_ORTHO){
            const float width = std::abs(camera.rightClip - camera.leftClip);
            const float height = std::abs(camera.topClip - camera.bottomClip);
            depth = std::max(width, height) / 2.0f;
        }
        if (std::isfinite(depth))
            farClip = std::min(farClip, nearClip + depth);
    }

    float nl, nr, nb, nt, fl, fr, fb, ft;
    if (camera.type == CameraType::CAMERA_PERSPECTIVE){
        const float tanHalfFov = std::tan(camera.yfov / 2.0f);
        const float hNear = std::abs(nearClip) * tanHalfFov;
        const float wNear = hNear * camera.aspect;
        const float hFar = std::abs(farClip) * tanHalfFov;
        const float wFar = hFar * camera.aspect;
        nl = -wNear; nr = wNear; nb = -hNear; nt = hNear;
        fl = -wFar; fr = wFar; fb = -hFar; ft = hFar;
        nearClip = std::abs(nearClip);
        farClip = std::abs(farClip);
    }else{
        nl = fl = camera.leftClip;
        nr = fr = camera.rightClip;
        nb = fb = camera.bottomClip;
        nt = ft = camera.topClip;
    }
    return {Vector3(nl, nt, -nearClip), Vector3(nr, nt, -nearClip), Vector3(nl, nb, -nearClip), Vector3(nr, nb, -nearClip),
            Vector3(fl, ft, -farClip), Vector3(fr, ft, -farClip), Vector3(fl, fb, -farClip), Vector3(fr, fb, -farClip)};
}

void editor::SceneRender::drawCameraFrustumLines(LineRenderer* lines, const CameraComponent& cameraComponent, bool isMainCamera, bool fullFrustum){
    lines->clearLines();

    if (cameraComponent.type == CameraType::CAMERA_UI){
        return;
    }

    Vector4 color(0.8f, 0.8f, 0.8f, 1.0f);
    if (isMainCamera){
        color = Vector4(0.5f, 1.0f, 0.5f, 1.0f);
    }

    const std::array<Vector3, 8> c = cameraGizmoCorners(cameraComponent, fullFrustum);
    const Vector3 &ntl = c[0], &ntr = c[1], &nbl = c[2], &nbr = c[3], &ftl = c[4], &ftr = c[5], &fbl = c[6], &fbr = c[7];

    lines->addLine(ntl, ntr, color);
    lines->addLine(ntr, nbr, color);
    lines->addLine(nbr, nbl, color);
    lines->addLine(nbl, ntl, color);

    lines->addLine(ftl, ftr, color);
    lines->addLine(ftr, fbr, color);
    lines->addLine(fbr, fbl, color);
    lines->addLine(fbl, ftl, color);

    lines->addLine(ntl, ftl, color);
    lines->addLine(ntr, ftr, color);
    lines->addLine(nbl, fbl, color);
    lines->addLine(nbr, fbr, color);

    if (cameraComponent.type == CameraType::CAMERA_PERSPECTIVE){
        // from the eye to the near plane
        lines->addLine(Vector3(0,0,0), ntl, color);
        lines->addLine(Vector3(0,0,0), ntr, color);
        lines->addLine(Vector3(0,0,0), nbl, color);
        lines->addLine(Vector3(0,0,0), nbr, color);
    }

    // Up marker, on the far plane's top edge
    const float halfHeight = std::abs(ftl.y - fbl.y) / 2.0f;
    const float markerSize = (cameraComponent.type == CameraType::CAMERA_PERSPECTIVE) ? halfHeight * 0.5f : halfHeight * 2.0f * 0.05f;
    const float cx = (ftl.x + ftr.x) / 2.0f;
    const float top = ftl.y;
    const float f = ftl.z;
    Vector3 up1(cx, top + markerSize, f);
    Vector3 up2(cx - markerSize / 2.0f, top, f);
    Vector3 up3(cx + markerSize / 2.0f, top, f);
    lines->addLine(up1, up2, color);
    lines->addLine(up2, up3, color);
    lines->addLine(up3, up1, color);
}

// the AI Navigation overlay of the scene this view shows (only built once a navigation
// component is in the scene: an empty scene pays nothing)
void editor::SceneRender::updateNavigationOverlay(const std::vector<Entity>& selEntities){
    const bool any = scene->getComponentArray<NavMeshSurfaceComponent>()->size() > 0 || scene->getComponentArray<NavGridComponent>()->size() > 0 ||
                     scene->getComponentArray<NavMeshLinkComponent>()->size() > 0 || scene->getComponentArray<NavMeshObstacleComponent>()->size() > 0 ||
                     scene->getComponentArray<NavMeshAgentComponent>()->size() > 0 || scene->getComponentArray<NavMeshModifierComponent>()->size() > 0;
    if (!any){
        if (navigationOverlay) navigationOverlay->hide();
        return;
    }
    Project* project = Backend::getApp().getProject();
    SceneProject* sceneProject = project ? project->getScene(getLatticeSceneId()) : nullptr;
    if (!sceneProject || sceneProject->scene != scene){
        return;
    }
    if (!navigationOverlay) navigationOverlay = std::make_unique<NavigationOverlay>(scene);
    const std::set<Entity> selected(selEntities.begin(), selEntities.end());
    navigationOverlay->update(*sceneProject, camera, selected, displaySettings.navigation);
}

// The wires of Wireframe and Shaded Wireframe: the visible meshes' edges at the level the Scene
// view draws each of them, rebuilt every update while the mode is on, released when it is not
void editor::SceneRender::updateWireframeOverlay(){
    if (!drawModeHasWires(displaySettings.drawMode) || !camera){
        if (wireframeLines){
            wireframeLines->clearLines();
            wireframeLines->setVisible(false);
        }
        wireframe.release();
        return;
    }
    if (!wireframeLines){
        ScopedDefaultEntityPool scenePool(*scene, EntityPool::System);
        wireframeLines = overlay(new LineRenderer(scene));
    }
    RenderSystem* render = scene->getSystem<RenderSystem>().get();
    CameraComponent& cameraComponent = scene->getComponent<CameraComponent>(camera->getEntity());
    const Vector3 eye = scene->getComponent<Transform>(camera->getEntity()).worldPosition;
    // in Edit mode the Scene view is the frame's view, while playing a view of its own
    const bool playing = isPlaying;
    wireframe.update(scene, wireframeLines, cameraComponent, eye, [&](Entity entity) -> unsigned int{
        if (playing)
            return render->getViewMeshLod(SCENE_VIEW_ID, entity);
        const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        return mesh ? mesh->lodFade.plan.dominant() : 0u;
    });
    wireframeLines->setVisible(true);
}
