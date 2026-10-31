// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SceneRender3D.h"
#include "pose/PoseViewport.h"

#include "resources/icons/sun-icon_png.h"
#include "resources/icons/bulb-icon_png.h"
#include "resources/icons/spot-icon_png.h"

#include "Project.h"
#include "LineDrawUtils.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace jasperengine;

editor::SceneRender3D::SceneRender3D(Scene* scene): SceneRender(scene, false, true, 40.0, 0.01){
    ScopedDefaultEntityPool sys(*scene, EntityPool::System);

    lastGridSpacing = 1.0f;

    lines = overlay(new LineRenderer(scene));

    camera->setNearClip(DEFAULT_EDITOR_CAMERA_NEAR);
    camera->setFarClip(DEFAULT_EDITOR_CAMERA_FAR);
    camera->setPosition(10, 4, 10);
    camera->setTarget(0, 0, 0);

    int linesStepChange = std::max(1, (int)(camera->getFarClip() / 2));
    linesOffset = Vector2(
        (int)(camera->getWorldPosition().x / linesStepChange) * linesStepChange,
        (int)(camera->getWorldPosition().z / linesStepChange) * linesStepChange
    );
    lastGridFarClip = camera->getFarClip();
    createLines();

    // Note: scene-appearance defaults (light state, global illumination, background
    // color) are applied when a new scene is created in Project::createScene(), not
    // here. Setting them in the constructor would clobber values loaded from disk,
    // since createSceneRender() runs after the scene has been decoded.

    uilayer.setViewportGizmoTexture(viewgizmo.getFramebuffer());

    poseViewport = std::make_unique<PoseViewport>(scene, toolslayer.getScene());
}

editor::SceneRender3D::~SceneRender3D(){
    // the pose overlays and ghosts first: they live in this scene and the tools layer
    poseViewport.reset();

    // a gizmo the scene destroyed is not deleted through an id that may be another entity's now
    dropGoneGizmos();

    delete lines;
    delete selLines;

    for (auto& pair : lightObjects) {
        delete pair.second.icon;
        delete pair.second.lines;
    }

    for (auto& pair : cameraObjects) {
        delete pair.second.icon;
        delete pair.second.lines;
    }

    for (auto& pair : soundObjects) {
        delete pair.second.icon;
    }

    for (auto& pair : bodyObjects) {
        delete pair.second.lines;
    }

    for (auto& pair : jointLines) {
        delete pair.second;
    }

    for (auto& pair : boneLines) {
        delete pair.second;
    }

    for (auto& pair : linePointLines) {
        delete pair.second;
    }

    for (auto& pair : polygonPointLines) {
        delete pair.second;
    }

    for (auto& pair : trackLines) {
        delete pair.second;
    }

    for (auto& pair : reflectionProbeLines) {
        delete pair.second;
    }
}

void editor::SceneRender3D::createLines(){
    float spacing = displaySettings.gridSpacing3D;
    if (spacing <= 0.0f) spacing = 1.0f;

    int gridHeight = 0;
    float gridSize = camera->getFarClip() * 2;

    float xGridStart = -gridSize + linesOffset.x;
    float xGridEnd = gridSize + linesOffset.x;

    float yGridStart = -gridSize + linesOffset.y;
    float yGridEnd = gridSize + linesOffset.y;

    lines->clearLines();

    int majorEvery = 10;

    float startX = std::floor(xGridStart / spacing) * spacing;
    for (float x = startX; x <= xGridEnd; x += spacing){
        int ix = (int)std::round(x / spacing);
        if (ix == 0){
            lines->addLine(Vector3(x, gridHeight, yGridStart), Vector3(x, gridHeight, yGridEnd), Vector4(0.5, 0.5, 1.0, 1.0));
        }else{
            if (ix % majorEvery == 0){
                lines->addLine(Vector3(x, gridHeight, yGridStart), Vector3(x, gridHeight, yGridEnd), Vector4(0.5, 0.5, 0.5, 1.0));
            }else{
                lines->addLine(Vector3(x, gridHeight, yGridStart), Vector3(x, gridHeight, yGridEnd), Vector4(0.5, 0.5, 0.5, 0.5));
            }
        }
    }

    float startZ = std::floor(yGridStart / spacing) * spacing;
    for (float z = startZ; z <= yGridEnd; z += spacing){
        int iz = (int)std::round(z / spacing);
        if (iz == 0){
            lines->addLine(Vector3(xGridStart, gridHeight, z), Vector3(xGridEnd, gridHeight, z), Vector4(1.0, 0.5, 0.5, 1.0));
        }else{
            if (iz % majorEvery == 0){
                lines->addLine(Vector3(xGridStart, gridHeight, z), Vector3(xGridEnd, gridHeight, z), Vector4(0.5, 0.5, 0.5, 1.0));
            }else{
                lines->addLine(Vector3(xGridStart, gridHeight, z), Vector3(xGridEnd, gridHeight, z), Vector4(0.5, 0.5, 0.5, 0.5));
            }
        }
    }
    lines->addLine(Vector3(0, -gridSize, 0), Vector3(0, gridSize, 0), Vector4(0.5, 1.0, 0.5, 1.0));
}

bool editor::SceneRender3D::instanciateLightObject(Entity entity){
    if (lightObjects.find(entity) == lightObjects.end()) {
        ScopedDefaultEntityPool sys(*scene, EntityPool::System);
        lightObjects[entity].icon = overlay(new Sprite(scene));
        lightObjects[entity].lines = overlay(new LineRenderer(scene));

        return true;
    }

    return false;
}

bool editor::SceneRender3D::instanciateCameraObject(Entity entity){
    if (cameraObjects.find(entity) == cameraObjects.end()) {
        ScopedDefaultEntityPool sys(*scene, EntityPool::System);
        cameraObjects[entity].icon = overlay(new Sprite(scene));
        cameraObjects[entity].lines = overlay(new LineRenderer(scene));

        return true;
    }

    return false;
}

bool editor::SceneRender3D::instanciateSoundObject(Entity entity){
    if (soundObjects.find(entity) == soundObjects.end()) {
        ScopedDefaultEntityPool sys(*scene, EntityPool::System);
        soundObjects[entity].icon = overlay(new Sprite(scene));

        return true;
    }

    return false;
}

bool editor::SceneRender3D::instanciateBodyObject(Entity entity){
    if (bodyObjects.find(entity) == bodyObjects.end()) {
        ScopedDefaultEntityPool sys(*scene, EntityPool::System);
        bodyObjects[entity].lines = overlay(new LineRenderer(scene));

        return true;
    }

    return false;
}

void editor::SceneRender3D::createOrUpdateReflectionProbeLines(Entity entity, const Transform& transform, const ReflectionProbeComponent& probe, bool visible){
    LineRenderer* probeLines = reflectionProbeLines[entity];
    probeLines->clearLines();
    probeLines->setVisible(transform.visible && visible);
    if (!transform.visible || !visible) return;

    Vector3 center = transform.modelMatrix * probe.boxOffset;
    Vector3 half(
        std::fabs(probe.boxSize.x * transform.worldScale.x) * 0.5f,
        std::fabs(probe.boxSize.y * transform.worldScale.y) * 0.5f,
        std::fabs(probe.boxSize.z * transform.worldScale.z) * 0.5f);
    Vector3 mn = center - half;
    Vector3 mx = center + half;
    Vector3 p[8] = {
        {mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z}, {mn.x, mx.y, mn.z},
        {mn.x, mn.y, mx.z}, {mx.x, mn.y, mx.z}, {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z}
    };
    const Vector4 color = probe.mode == ReflectionProbeMode::DYNAMIC
        ? Vector4(0.8f, 0.35f, 1.0f, 1.0f)
        : Vector4(0.2f, 0.8f, 1.0f, 1.0f);
    const int edges[12][2] = {
        {0,1}, {1,2}, {2,3}, {3,0}, {4,5}, {5,6}, {6,7}, {7,4},
        {0,4}, {1,5}, {2,6}, {3,7}
    };
    for (const auto& edge : edges) probeLines->addLine(p[edge[0]], p[edge[1]], color);

    float marker = std::max(0.05f, std::min(half.x, std::min(half.y, half.z)) * 0.08f);
    probeLines->addLine(center - Vector3(marker, 0, 0), center + Vector3(marker, 0, 0), color);
    probeLines->addLine(center - Vector3(0, marker, 0), center + Vector3(0, marker, 0), color);
    probeLines->addLine(center - Vector3(0, 0, marker), center + Vector3(0, 0, marker), color);

    // boxOffset moves only the influence volume. When its center differs from
    // the actual capture point, show the entity origin as a smaller gold marker.
    Vector3 capturePosition = transform.worldPosition;
    if ((capturePosition - center).length() > 0.0001f){
        float captureMarker = marker * 0.65f;
        const Vector4 captureColor(1.0f, 0.72f, 0.15f, 1.0f);
        probeLines->addLine(capturePosition - Vector3(captureMarker, 0, 0), capturePosition + Vector3(captureMarker, 0, 0), captureColor);
        probeLines->addLine(capturePosition - Vector3(0, captureMarker, 0), capturePosition + Vector3(0, captureMarker, 0), captureColor);
        probeLines->addLine(capturePosition - Vector3(0, 0, captureMarker), capturePosition + Vector3(0, 0, captureMarker), captureColor);
    }
}

// endpoint handles for a selected Lines entity, drawn as small 3-axis crosses
// (orientation-independent in 3D); the sub-selected endpoint is drawn bigger
// and in the selection color
void editor::SceneRender3D::createOrUpdateLinePointLines(Entity entity, const Transform& transform, const LineRendererComponent& lines, bool visible){
    LineRenderer* handlesObj = linePointLines[entity];

    handlesObj->clearLines();
    handlesObj->setVisible(transform.visible && visible);

    if (!transform.visible || !visible){
        return;
    }

    const Vector4 handleColor(1.0f, 1.0f, 1.0f, 1.0f);
    const Vector4 selectedHandleColor(1.0f, 0.6f, 0.0f, 1.0f);
    bool hasPointSelection = (getSelectedLinePointEntity() == entity);

    for (size_t i = 0; i < lines.lines.size(); i++){
        for (int e = 0; e < 2; e++){
            const Vector3& localPoint = (e == 0) ? lines.lines[i].pointA : lines.lines[i].pointB;
            Vector3 worldPoint = transform.modelMatrix * localPoint;

            bool pointSelected = hasPointSelection && ((int)(i * 2) + e == getSelectedLinePointIndex());
            const Vector4& color = pointSelected ? selectedHandleColor : handleColor;
            float halfSize = getPointHandleHalfSize(worldPoint) * (pointSelected ? 0.75f : 0.5f);

            handlesObj->addLine(worldPoint - Vector3(halfSize, 0, 0), worldPoint + Vector3(halfSize, 0, 0), color);
            handlesObj->addLine(worldPoint - Vector3(0, halfSize, 0), worldPoint + Vector3(0, halfSize, 0), color);
            handlesObj->addLine(worldPoint - Vector3(0, 0, halfSize), worldPoint + Vector3(0, 0, halfSize), color);
        }
    }
}

// vertex handles for a selected Polygon / MeshPolygon entity, drawn as small
// 3-axis crosses; the sub-selected vertex is drawn bigger and in the selection color
void editor::SceneRender3D::createOrUpdatePolygonPointLines(Entity entity, const Transform& transform, const std::vector<PolygonPoint>& points, bool isMesh, bool visible){
    LineRenderer* handlesObj = polygonPointLines[entity];

    handlesObj->clearLines();
    handlesObj->setVisible(transform.visible && visible);

    if (!transform.visible || !visible){
        return;
    }

    const Vector4 handleColor(1.0f, 1.0f, 1.0f, 1.0f);
    const Vector4 selectedHandleColor(1.0f, 0.6f, 0.0f, 1.0f);
    bool hasPointSelection = (getSelectedPolygonPointEntity() == entity && isSelectedPolygonPointMesh() == isMesh);

    for (size_t i = 0; i < points.size(); i++){
        Vector3 worldPoint = transform.modelMatrix * points[i].position;

        bool pointSelected = hasPointSelection && ((int)i == getSelectedPolygonPointIndex());
        const Vector4& color = pointSelected ? selectedHandleColor : handleColor;
        float halfSize = getPointHandleHalfSize(worldPoint) * (pointSelected ? 0.75f : 0.5f);

        handlesObj->addLine(worldPoint - Vector3(halfSize, 0, 0), worldPoint + Vector3(halfSize, 0, 0), color);
        handlesObj->addLine(worldPoint - Vector3(0, halfSize, 0), worldPoint + Vector3(0, halfSize, 0), color);
        handlesObj->addLine(worldPoint - Vector3(0, 0, halfSize), worldPoint + Vector3(0, 0, halfSize), color);
    }
}

bool editor::SceneRender3D::instanciateTrackLines(Entity entity){
    if (trackLines.find(entity) == trackLines.end()) {
        // tools-layer scene so the path stays visible on top of scene geometry
        // (waypoints usually sit exactly on floors/meshes), like bone lines
        ScopedDefaultEntityPool sys(*toolslayer.getScene(), EntityPool::System);
        trackLines[entity] = new LineRenderer(toolslayer.getScene());

        return true;
    }

    return false;
}

// path of a selected TranslateTracks entity: a polyline through its keyframe
// values (in the action target's parent space) with small 3-axis cross handles;
// the sub-selected point is drawn bigger and in the selection color
void editor::SceneRender3D::createOrUpdateTrackLines(Entity entity, const TranslateTracksComponent& tracks, bool visible){
    LineRenderer* trackObj = trackLines[entity];

    trackObj->clearLines();
    trackObj->setVisible(visible);

    if (!visible){
        return;
    }

    const Vector4 pathColor(0.4f, 0.75f, 1.0f, 1.0f);
    const Vector4 handleColor(1.0f, 1.0f, 1.0f, 1.0f);
    const Vector4 selectedHandleColor(1.0f, 0.6f, 0.0f, 1.0f);
    bool hasPointSelection = (getSelectedTrackPointEntity() == entity);

    Matrix4 worldMatrix = getTrackPointsWorldMatrix(entity);

    Vector3 prevWorldPoint;
    for (size_t i = 0; i < tracks.values.size(); i++){
        Vector3 worldPoint = worldMatrix * tracks.values[i];

        if (i > 0){
            trackObj->addLine(prevWorldPoint, worldPoint, pathColor);
        }
        prevWorldPoint = worldPoint;

        bool pointSelected = hasPointSelection && ((int)i == getSelectedTrackPointIndex());
        const Vector4& color = pointSelected ? selectedHandleColor : handleColor;
        float halfSize = getPointHandleHalfSize(worldPoint) * (pointSelected ? 0.75f : 0.5f);

        trackObj->addLine(worldPoint - Vector3(halfSize, 0, 0), worldPoint + Vector3(halfSize, 0, 0), color);
        trackObj->addLine(worldPoint - Vector3(0, halfSize, 0), worldPoint + Vector3(0, halfSize, 0), color);
        trackObj->addLine(worldPoint - Vector3(0, 0, halfSize), worldPoint + Vector3(0, 0, halfSize), color);
    }
}

bool editor::SceneRender3D::instanciateBoneLines(Entity entity){
    if (boneLines.find(entity) == boneLines.end()) {
        ScopedDefaultEntityPool sys(*toolslayer.getScene(), EntityPool::System);
        boneLines[entity] = new LineRenderer(toolslayer.getScene());

        return true;
    }

    return false;
}

void editor::SceneRender3D::createOrUpdateBoneLines(Entity entity, const ModelComponent& model, bool visible, bool highlighted){
    LineRenderer* boneLinesObj = boneLines[entity];

    boneLinesObj->clearLines();
    boneLinesObj->setVisible(visible);

    if (!visible){
        return;
    }

    float alpha = highlighted ? 1.0f : 0.5f;
    const Vector4 boneColor(0.9f, 0.9f, 0.2f, alpha);
    const Vector4 tipColor(1.0f, 0.5f, 0.1f, alpha);

    for (const auto& [boneId, boneEntity] : model.bonesIdMapping) {
        Transform* boneTransform = scene->findComponent<Transform>(boneEntity);
        if (!boneTransform || !boneTransform->visible) continue;

        Vector3 bonePos = boneTransform->worldPosition;

        Entity parentEntity = boneTransform->parent;
        if (parentEntity != NULL_ENTITY) {
            BoneComponent* parentBone = scene->findComponent<BoneComponent>(parentEntity);
            if (parentBone) {
                Transform* parentTransform = scene->findComponent<Transform>(parentEntity);
                if (parentTransform) {
                    Vector3 parentPos = parentTransform->worldPosition;
                    Vector3 dir = bonePos - parentPos;
                    float length = dir.length();

                    if (length < 1e-6f) continue;

                    dir = dir / length;

                    // Build perpendicular basis
                    Vector3 up = (std::abs(dir.y) < 0.99f) ? Vector3(0, 1, 0) : Vector3(1, 0, 0);
                    Vector3 right = dir.crossProduct(up).normalized();
                    up = right.crossProduct(dir).normalized();

                    // Wireframe pyramid: base near parent, tip at child
                    float baseOffset = length * 0.15f;
                    float baseSize = length * 0.08f;
                    Vector3 baseCenter = parentPos + dir * baseOffset;

                    Vector3 b0 = baseCenter + (right + up) * baseSize;
                    Vector3 b1 = baseCenter + (right - up) * baseSize;
                    Vector3 b2 = baseCenter + (right * -1.0f - up) * baseSize;
                    Vector3 b3 = baseCenter + (up - right) * baseSize;

                    // Base square
                    boneLinesObj->addLine(b0, b1, boneColor);
                    boneLinesObj->addLine(b1, b2, boneColor);
                    boneLinesObj->addLine(b2, b3, boneColor);
                    boneLinesObj->addLine(b3, b0, boneColor);

                    // Lines from parent to base
                    boneLinesObj->addLine(parentPos, b0, tipColor);
                    boneLinesObj->addLine(parentPos, b1, tipColor);
                    boneLinesObj->addLine(parentPos, b2, tipColor);
                    boneLinesObj->addLine(parentPos, b3, tipColor);

                    // Lines from base to child tip
                    boneLinesObj->addLine(b0, bonePos, boneColor);
                    boneLinesObj->addLine(b1, bonePos, boneColor);
                    boneLinesObj->addLine(b2, bonePos, boneColor);
                    boneLinesObj->addLine(b3, bonePos, boneColor);
                }
            }
        } else {
            // Root bone: draw a small octahedron marker
            float markSize = 0.03f;
            boneLinesObj->addLine(bonePos + Vector3(0, markSize, 0), bonePos + Vector3(markSize, 0, 0), tipColor);
            boneLinesObj->addLine(bonePos + Vector3(markSize, 0, 0), bonePos + Vector3(0, -markSize, 0), tipColor);
            boneLinesObj->addLine(bonePos + Vector3(0, -markSize, 0), bonePos + Vector3(-markSize, 0, 0), tipColor);
            boneLinesObj->addLine(bonePos + Vector3(-markSize, 0, 0), bonePos + Vector3(0, markSize, 0), tipColor);

            boneLinesObj->addLine(bonePos + Vector3(0, markSize, 0), bonePos + Vector3(0, 0, markSize), tipColor);
            boneLinesObj->addLine(bonePos + Vector3(0, 0, markSize), bonePos + Vector3(0, -markSize, 0), tipColor);
            boneLinesObj->addLine(bonePos + Vector3(0, -markSize, 0), bonePos + Vector3(0, 0, -markSize), tipColor);
            boneLinesObj->addLine(bonePos + Vector3(0, 0, -markSize), bonePos + Vector3(0, markSize, 0), tipColor);
        }
    }
}

void editor::SceneRender3D::createOrUpdateBodyLines(Entity entity, const Transform& transform, const RigidbodyComponent& body, bool visible, bool highlighted) {
    BodyObjects& bo = bodyObjects[entity];

    bo.lines->clearLines();
    bo.lines->setVisible(transform.visible && visible);

    if (!transform.visible || !visible || body.colliders.empty()) {
        return;
    }

    float alpha = highlighted ? 1.0f : 0.35f;
    const Vector4 bodyColor(0.2f, 0.95f, 0.95f, alpha);
    const Vector4 triggerColor(1.0f, 0.9f, 0.15f, alpha);
    const Vector4 centerColor(1.0f, 0.4f, 0.1f, alpha);

    // the same wireframes as the runtime debug draw (PhysicsDebugLines), at the Transform
    PhysicsDebugLines::Pose pose;
    pose.position = transform.worldPosition;
    pose.rotation = transform.worldRotation;
    pose.scale = transform.worldScale;
    PhysicsDebugLines::bodyEdges(scene, entity, transform, body, pose, bo.shapeEdgeCaches,
        [&](const Vector3& a, const Vector3& b, size_t collider) {
            bo.lines->addLine(a, b, (collider < body.colliders.size() && body.colliders[collider].isTrigger) ? triggerColor : bodyColor);
        });

    // the centre of mass of the selected body: given, or computed from primitive colliders
    // (mesh-sourced ones would be built every frame)
    if (highlighted) {
        bool hasCenter = false;
        Vector3 center = body.centerOfMass;
        if (!body.automaticCenterOfMass) {
            hasCenter = true;
        } else {
            bool primitives = true;
            for (const Collider3D& collider : body.colliders) {
                if (collider.type == Collider3DType::MESH || collider.type == Collider3DType::HEIGHTFIELD ||
                        (collider.type == Collider3DType::CONVEX_HULL && collider.source != Collider3DSource::RAW_VERTICES)) {
                    primitives = false;
                }
            }
            PhysicsSystem::MassInfo3D info;
            if (primitives && scene->getSystem<PhysicsSystem>()->computeMassProperties3D(entity, body, info)) {
                center = info.centerOfMass;
                hasCenter = true;
            }
        }
        if (hasCenter) {
            const Vector3 world = transform.worldPosition + transform.worldRotation * center;
            PhysicsDebugLines::crossMark(world, 0.08f, [&](const Vector3& a, const Vector3& b) { bo.lines->addLine(a, b, centerColor); });
            LineDrawUtils::addRing3D(bo.lines, world, Vector3(0, 1, 0), 0.06f, centerColor, 12);
            LineDrawUtils::addRing3D(bo.lines, world, Vector3(1, 0, 0), 0.06f, centerColor, 12);
        }
    }
}

void editor::SceneRender3D::createOrUpdateJointLines(Entity entity, const Joint3DComponent& joint, bool visible, bool highlighted){
    LineRenderer* jointLinesObj = jointLines[entity];

    jointLinesObj->clearLines();
    jointLinesObj->setVisible(visible);

    if (!visible){
        return;
    }

    float alpha = highlighted ? 1.0f : 0.35f;

    const Vector4 jointColor(0.95f, 0.75f, 0.2f, alpha);
    const Vector4 helperColor(0.85f, 0.85f, 0.85f, 0.8f * alpha);
    const Vector4 axisColor(0.2f, 0.9f, 1.0f, alpha);
    const Vector4 limitColor(1.0f, 0.35f, 0.35f, alpha);

    // the bodies at their Transforms (LOCAL anchors are points of them)
    auto poseOf = [&](Entity bodyEntity, PhysicsDebugLines::Pose& pose) -> bool {
        Transform* transform = scene->findComponent<Transform>(bodyEntity);
        if (!transform) return false;
        pose.position = transform->worldPosition;
        pose.rotation = transform->worldRotation;
        pose.scale = transform->worldScale;
        return true;
    };
    PhysicsDebugLines::jointEdges(scene, joint, poseOf, [&](const Vector3& a, const Vector3& b, int kind) {
        const Vector4& color = kind == 0 ? jointColor : (kind == 1 ? helperColor : (kind == 2 ? axisColor : limitColor));
        jointLinesObj->addLine(a, b, color);
    });
}

void editor::SceneRender3D::createOrUpdateLightIcon(Entity entity, const Transform& transform, LightType lightType, bool newLight) {
    LightObjects& lo = lightObjects[entity];

    if (newLight) {
        TextureData iconData;
        if (lightType == LightType::DIRECTIONAL) {
            iconData.loadTextureFromMemory(sun_icon_png, sun_icon_png_len);
            lo.icon->setTexture("editor:resources:sun_icon", iconData);
        } else if (lightType == LightType::POINT) {
            iconData.loadTextureFromMemory(bulb_icon_png, bulb_icon_png_len);
            lo.icon->setTexture("editor:resources:bulb_icon", iconData);
        } else if (lightType == LightType::SPOT) {
            iconData.loadTextureFromMemory(spot_icon_png, spot_icon_png_len);
            lo.icon->setTexture("editor:resources:spot_icon", iconData);
        }

        lo.icon->setBillboard(true);
        lo.icon->setSize(128, 128);
        lo.icon->setReceiveLights(false);
        lo.icon->setCastShadows(false);
        lo.icon->setReceiveShadows(false);
        lo.icon->setPivotPreset(PivotPreset::CENTER);
    }

    // Update light icon position
    lo.icon->setPosition(transform.worldPosition);
    lo.icon->setVisible(transform.visible);
    lo.icon->setScale(billboardScreenScale(transform.worldPosition, 0.25f));
}

void editor::SceneRender3D::createOrUpdateCameraIcon(Entity entity, const Transform& transform, bool newCamera) {
    CameraObjects& co = cameraObjects[entity];

    if (newCamera) {
        setupCameraIcon(co);
        co.icon->setBillboard(true);
    }

    // Update camera icon position
    co.icon->setPosition(transform.worldPosition);
    co.icon->setVisible(transform.visible);
    co.icon->setScale(billboardScreenScale(transform.worldPosition, 0.25f));
}

void editor::SceneRender3D::createOrUpdateSoundIcon(Entity entity, const Transform& transform, bool newSound) {
    SoundObjects& so = soundObjects[entity];

    if (newSound) {
        setupSoundIcon(so);
        so.icon->setBillboard(true);
    }

    so.icon->setPosition(transform.worldPosition);
    so.icon->setVisible(transform.visible);
    so.icon->setScale(billboardScreenScale(transform.worldPosition, 0.25f));
}

void editor::SceneRender3D::createCameraFrustum(Entity entity, const Transform& transform, const CameraComponent& cameraComponent, bool fixedSizeFrustum, bool isMainCamera) {
    CameraObjects& co = cameraObjects[entity];

    co.lines->setPosition(transform.worldPosition);
    co.lines->setVisible(transform.visible);

    Quaternion rotation;
    rotation.fromRotationMatrix(cameraComponent.viewMatrix.inverse());
    co.lines->setRotation(rotation);

    updateCameraFrustum(co, cameraComponent, isMainCamera, fixedSizeFrustum);
}


void editor::SceneRender3D::createDirectionalLightArrow(Entity entity, const Transform& transform, const LightComponent& light, bool isSelected) {
    LightObjects& lo = lightObjects[entity];

    lo.lines->setPosition(transform.worldPosition);
    lo.lines->setRotation(transform.worldRotation);
    lo.lines->setVisible(isSelected);

    if (light.direction == Vector3::ZERO){
        return;
    }

    if (lo.type == light.type && lo.direction == light.direction) {
        return;
    }

    lo.type = light.type;
    lo.direction = light.direction;

    lo.lines->clearLines();

    Vector3 position = Vector3(0, 0, 0);  // Start position
    float arrowLength = 4.0f;  // Length of the main arrow shaft
    float arrowHeadLength = 3.6f;  // Length of the arrow head
    float arrowHeadWidth = 1.0f;   // Width of the arrow head

    Vector4 arrowColor = Vector4(1.0, 1.0, 0.0, 1.0); // Yellow color for directional light
    Vector3 direction = light.direction.normalized();

    // Main arrow shaft
    Vector3 endPos = position + direction * arrowLength;
    lo.lines->addLine(position, endPos, arrowColor);

    // Use the same local projection frame as runtime lighting and shadows.
    Vector3 up = light.getLocalSpotProjectionUp();
    Vector3 right = direction.crossProduct(up).normalized();

    // Arrow head base position
    Vector3 arrowHeadBase = endPos - direction * arrowHeadLength;

    // Arrow head vertices (4 points forming a diamond shape)
    Vector3 arrowHead1 = arrowHeadBase + right * arrowHeadWidth;
    Vector3 arrowHead2 = arrowHeadBase + up * arrowHeadWidth;
    Vector3 arrowHead3 = arrowHeadBase - right * arrowHeadWidth;
    Vector3 arrowHead4 = arrowHeadBase - up * arrowHeadWidth;

    // Draw arrow head lines
    lo.lines->addLine(endPos, arrowHead1, arrowColor);
    lo.lines->addLine(endPos, arrowHead2, arrowColor);
    lo.lines->addLine(endPos, arrowHead3, arrowColor);
    lo.lines->addLine(endPos, arrowHead4, arrowColor);

    // Connect arrow head base points to form a diamond
    lo.lines->addLine(arrowHead1, arrowHead2, arrowColor);
    lo.lines->addLine(arrowHead2, arrowHead3, arrowColor);
    lo.lines->addLine(arrowHead3, arrowHead4, arrowColor);
    lo.lines->addLine(arrowHead4, arrowHead1, arrowColor);

    // Optional: Add some additional lines for better visualization
    lo.lines->addLine(arrowHead1, arrowHead3, arrowColor * 0.7f); // Cross lines with slightly dimmer color
    lo.lines->addLine(arrowHead2, arrowHead4, arrowColor * 0.7f);
}

void editor::SceneRender3D::createPointLightSphere(Entity entity, const Transform& transform, const LightComponent& light, bool isSelected) {
    LightObjects& lo = lightObjects[entity];

    lo.lines->setPosition(transform.worldPosition);
    lo.lines->setRotation(transform.worldRotation);
    lo.lines->setVisible(isSelected);

    float range = (light.range > 0.0f) ? light.range : camera->getFarClip();

    if (lo.type == light.type && lo.range == range) {
        return;
    }

    lo.type = light.type;
    lo.range = range;

    lo.lines->clearLines();

    Vector3 position = Vector3(0, 0, 0);  // Start position
    Vector4 sphereColor = Vector4(0.0, 1.0, 1.0, 0.8); // Cyan color for point light

    const int numSegments = 16;
    const float angleStep = 2.0f * M_PI / numSegments;

    // Draw three orthogonal circles to represent the sphere

    // XY plane circle
    for (int i = 0; i < numSegments; i++) {
        float angle1 = i * angleStep;
        float angle2 = ((i + 1) % numSegments) * angleStep;

        Vector3 point1 = position + Vector3(std::cos(angle1) * range, std::sin(angle1) * range, 0);
        Vector3 point2 = position + Vector3(std::cos(angle2) * range, std::sin(angle2) * range, 0);

        lo.lines->addLine(point1, point2, sphereColor);
    }

    // XZ plane circle
    for (int i = 0; i < numSegments; i++) {
        float angle1 = i * angleStep;
        float angle2 = ((i + 1) % numSegments) * angleStep;

        Vector3 point1 = position + Vector3(std::cos(angle1) * range, 0, std::sin(angle1) * range);
        Vector3 point2 = position + Vector3(std::cos(angle2) * range, 0, std::sin(angle2) * range);

        lo.lines->addLine(point1, point2, sphereColor);
    }

    // YZ plane circle
    for (int i = 0; i < numSegments; i++) {
        float angle1 = i * angleStep;
        float angle2 = ((i + 1) % numSegments) * angleStep;

        Vector3 point1 = position + Vector3(0, std::cos(angle1) * range, std::sin(angle1) * range);
        Vector3 point2 = position + Vector3(0, std::cos(angle2) * range, std::sin(angle2) * range);

        lo.lines->addLine(point1, point2, sphereColor);
    }
}

void editor::SceneRender3D::createSpotLightCones(Entity entity, const Transform& transform, const LightComponent& light, bool isSelected) {
    LightObjects& lo = lightObjects[entity];

    lo.lines->setPosition(transform.worldPosition);
    lo.lines->setRotation(transform.worldRotation);
    lo.lines->setVisible(isSelected);

    if (light.direction == Vector3::ZERO){
        return;
    }

    float range = (light.range > 0.0f) ? light.range : camera->getFarClip();

    // if light.range = 0.0 then light.shadowCameraNearFar.y is camera.farClip
    if (lo.type == light.type && 
        lo.innerConeCos == light.innerConeCos && 
        lo.outerConeCos == light.outerConeCos && 
        lo.direction == light.direction && 
        lo.range == range &&
        lo.spotMaskReady == light.spotMaskReady &&
        lo.spotMaskAspect == light.spotMaskAspect) {
        return;
    }

    lo.type = light.type;
    lo.innerConeCos = light.innerConeCos;
    lo.outerConeCos = light.outerConeCos;
    lo.direction = light.direction;
    lo.range = range;
    lo.spotMaskReady = light.spotMaskReady;
    lo.spotMaskAspect = light.spotMaskAspect;

    lo.lines->clearLines();

    Vector3 position = Vector3(0,0,0);  // Start position
    Vector3 direction = light.direction.normalized();

    // Calculate cone radii at the end of the light range
    float innerRadius = range * std::tan(std::acos(light.innerConeCos));
    float outerRadius = range * std::tan(std::acos(light.outerConeCos));

    // Use the same local projection frame as runtime lighting and shadows.
    Vector3 up = light.getLocalSpotProjectionUp();
    Vector3 right = direction.crossProduct(up).normalized();

    // End position of the cone
    Vector3 endPos = position + direction * range;

    const int numSegments = 12;
    const float angleStep = 2.0f * M_PI / numSegments;

    // Colors for inner and outer cones
    Vector4 innerConeColor = Vector4(1.0, 1.0, 0.0, 0.8); // Yellow for inner cone
    Vector4 outerConeColor = Vector4(1.0, 0.5, 0.0, 0.6); // Orange for outer cone

    if (light.spotMaskReady){
        float outerHalfWidth = outerRadius * light.spotMaskAspect;
        Vector3 corners[4] = {
            endPos - right * outerHalfWidth - up * outerRadius,
            endPos + right * outerHalfWidth - up * outerRadius,
            endPos + right * outerHalfWidth + up * outerRadius,
            endPos - right * outerHalfWidth + up * outerRadius
        };

        for (int i = 0; i < 4; i++){
            lo.lines->addLine(position, corners[i], outerConeColor);
            lo.lines->addLine(corners[i], corners[(i + 1) % 4], outerConeColor);
        }
        lo.lines->addLine(position, endPos, Vector4(0.8, 0.8, 0.8, 1.0));
        return;
    }

    // Draw outer cone
    for (int i = 0; i < numSegments; i++) {
        float angle1 = i * angleStep;
        float angle2 = ((i + 1) % numSegments) * angleStep;

        // Calculate points on the outer cone circle
        Vector3 point1 = endPos + (right * std::cos(angle1) + up * std::sin(angle1)) * outerRadius;
        Vector3 point2 = endPos + (right * std::cos(angle2) + up * std::sin(angle2)) * outerRadius;

        // Lines from light position to circle points
        lo.lines->addLine(position, point1, outerConeColor);

        // Circle at the end of the cone
        lo.lines->addLine(point1, point2, outerConeColor);
    }

    // Draw inner cone
    for (int i = 0; i < numSegments; i++) {
        float angle1 = i * angleStep;
        float angle2 = ((i + 1) % numSegments) * angleStep;

        // Calculate points on the inner cone circle
        Vector3 point1 = endPos + (right * std::cos(angle1) + up * std::sin(angle1)) * innerRadius;
        Vector3 point2 = endPos + (right * std::cos(angle2) + up * std::sin(angle2)) * innerRadius;

        // Lines from light position to circle points
        lo.lines->addLine(position, point1, innerConeColor);

        // Circle at the end of the cone
        lo.lines->addLine(point1, point2, innerConeColor);
    }

    // Draw central direction line
    lo.lines->addLine(position, endPos, Vector4(0.8, 0.8, 0.8, 1.0));
}

void editor::SceneRender3D::dropGoneGizmos(){
    SceneRender::dropGoneGizmos();

    const RenderSystem& render = *scene->getSystem<RenderSystem>();
    dropGoneGizmos(lightObjects, render, &LightObjects::icon, &LightObjects::lines);
    dropGoneGizmos(cameraObjects, render, &CameraObjects::icon, &CameraObjects::lines);
    dropGoneGizmos(soundObjects, render, &SoundObjects::icon);
    dropGoneGizmos(bodyObjects, render, &BodyObjects::lines);
    dropGoneGizmos(jointLines, render);
    dropGoneGizmos(boneLines, render);
    dropGoneGizmos(linePointLines, render);
    dropGoneGizmos(polygonPointLines, render);
    dropGoneGizmos(trackLines, render);
    dropGoneGizmos(reflectionProbeLines, render);
}

void editor::SceneRender3D::hideAllGizmos(){
    SceneRender::hideAllGizmos();

    lines->setVisible(false);
    for (auto& pair : lightObjects) {
        pair.second.icon->setVisible(false);
        pair.second.lines->setVisible(false);
    }
    for (auto& pair : cameraObjects) {
        pair.second.icon->setVisible(false);
        pair.second.lines->setVisible(false);
    }
    for (auto& pair : soundObjects) {
        pair.second.icon->setVisible(false);
    }
    for (auto& pair : bodyObjects) {
        pair.second.lines->setVisible(false);
    }
    for (auto& pair : jointLines) {
        pair.second->setVisible(false);
    }
    for (auto& pair : boneLines) {
        pair.second->setVisible(false);
    }
    for (auto& pair : linePointLines) {
        pair.second->setVisible(false);
    }
    for (auto& pair : polygonPointLines) {
        pair.second->setVisible(false);
    }
    for (auto& pair : trackLines) {
        pair.second->setVisible(false);
    }
    for (auto& pair : reflectionProbeLines) {
        pair.second->setVisible(false);
    }
}

void editor::SceneRender3D::activate(){
    SceneRender::activate();

    if (!isPreviewCameraActive()){
        Engine::addSceneLayer(viewgizmo.getScene());
    }
}

// the orientation gizmo the overlay layer shows renders into its own framebuffer first
void editor::SceneRender3D::drawOverlayTargets(){
    Scene* gizmoScene = viewgizmo.getScene();
    Framebuffer* gizmoTarget = viewgizmo.getFramebuffer();
    gizmoScene->update(Engine::getDeltatime());
    if (!gizmoTarget->isCreated()){
        gizmoTarget->create();
    }
    Engine::drawView(SCENE_VIEW_ID, gizmoTarget, {{gizmoScene, gizmoScene->getCamera(),
        Rect(0, 0, (float)gizmoTarget->getWidth(), (float)gizmoTarget->getHeight()), false, false}});
}

void editor::SceneRender3D::setOverlayScale(float scale){
    float previous = overlayScale;
    SceneRender::setOverlayScale(scale);
    if (overlayScale == previous){
        return;
    }
    viewgizmo.setFramebufferSize(std::max(128, (int)std::lround(100.0f * overlayScale)));
}

void editor::SceneRender3D::zoomCamera(float amount){
    if (amount == 0.0f){
        return;
    }

    float distance = camera->getDistanceFromTarget();
    float newDistance = std::clamp(distance - amount, MIN_EDITOR_CAMERA_DISTANCE, MAX_EDITOR_CAMERA_DISTANCE);

    camera->zoom(distance - newDistance);
}

float editor::SceneRender3D::getWalkSpeedOffset() const{
    return walkSpeedOffset;
}

void editor::SceneRender3D::setWalkSpeedOffset(float offset){
    walkSpeedOffset = std::clamp(
        offset,
        MIN_EDITOR_WALK_SPEED - DEFAULT_EDITOR_WALK_SPEED,
        MAX_EDITOR_WALK_SPEED - DEFAULT_EDITOR_WALK_SPEED);
}

void editor::SceneRender3D::updateSelLines(std::vector<OBB> obbs){
    fillSelectionLines(selLines, obbs, true);
}

void editor::SceneRender3D::update(std::vector<Entity> selEntities, std::vector<Entity> entities, Entity mainCamera, const SceneDisplaySettings& settings){
    SceneRender::update(selEntities, entities, mainCamera, settings);

    // pose animation handles, floor and ghosts (hidden while playing or previewing a camera)
    poseViewport->update(camera, getViewWidth(), getViewHeight(), overlayScale, isPlaying || isPreviewCameraActive());

    if (isPreviewCameraActive()){
        return;
    }

    lines->setVisible(displaySettings.showGrid3D);

    float spacing = displaySettings.gridSpacing3D;
    if (spacing <= 0.0f) spacing = 1.0f;

    int linesStepChange = std::max(1, (int)(camera->getFarClip() / 2));
    int cameraLineStepX = (int)(camera->getWorldPosition().x / linesStepChange) * linesStepChange;
    int cameraLineStepZ = (int)(camera->getWorldPosition().z / linesStepChange) * linesStepChange;
    bool spacingChanged = (lastGridSpacing != spacing);
    bool farClipChanged = (lastGridFarClip != camera->getFarClip());
    if (cameraLineStepX != linesOffset.x || cameraLineStepZ != linesOffset.y || spacingChanged || farClipChanged){
        linesOffset = Vector2(cameraLineStepX, cameraLineStepZ);
        lastGridSpacing = spacing;
        lastGridFarClip = camera->getFarClip();

        createLines();
    }

    viewgizmo.applyRotation(camera);

    uilayer.updateCameraGauge(camera->getDistanceFromTarget(), MIN_EDITOR_CAMERA_DISTANCE, MAX_EDITOR_CAMERA_DISTANCE);

    std::set<Entity> selectedEntities(selEntities.begin(), selEntities.end());

    // Several overlay types ask whether the same entity contains a selected child.
    // Build that answer once per update instead of scanning the Transform array in
    // isParentOf() for every body, line, polygon, joint and model. Keep the array
    // order check used by isParentOf(), including its direct-parent exception.
    std::unordered_set<Entity> selectedAncestors;
    auto transforms = scene->getComponentArray<Transform>();
    for (Entity selected : selectedEntities){
        selectedAncestors.insert(selected);
        Transform* selectedTransform = scene->findComponent<Transform>(selected);
        if (!selectedTransform) continue;

        size_t scanLimit = transforms->getIndex(selected);
        Entity parent = selectedTransform->parent;
        while (parent != NULL_ENTITY){
            Transform* parentTransform = scene->findComponent<Transform>(parent);
            if (!parentTransform) break;

            selectedAncestors.insert(parent);
            size_t parentIndex = transforms->getIndex(parent);
            if (parentIndex >= scanLimit) break;
            scanLimit = parentIndex;
            parent = parentTransform->parent;
        }
    }

    auto isDescendantSelected = [&](Entity ancestor) -> bool {
        return selectedAncestors.find(ancestor) != selectedAncestors.end();
    };

    std::set<Entity> currentIconLights;
    std::set<Entity> currentIconCameras;
    std::set<Entity> currentIconSounds;
    std::set<Entity> currentBodyObjects;
    std::set<Entity> currentJointObjects;
    std::set<Entity> currentBoneModels;
    std::set<Entity> currentLinePoints;
    std::set<Entity> currentPolygonPoints;
    std::set<Entity> currentTrackLines;
    std::set<Entity> currentReflectionProbes;

    for (Entity& entity: entities){
        Signature signature = scene->getSignature(entity);

        if (signature.test(scene->getComponentId<LightComponent>()) && signature.test(scene->getComponentId<Transform>())) {
            LightComponent& light = scene->getComponent<LightComponent>(entity);
            Transform& transform = scene->getComponent<Transform>(entity);

            bool isSelected = selectedEntities.find(entity) != selectedEntities.end();

            currentIconLights.insert(entity);
            bool newLight = instanciateLightObject(entity) || lightObjects[entity].type != light.type;
            if (light.type == LightType::DIRECTIONAL){
                createOrUpdateLightIcon(entity, transform, LightType::DIRECTIONAL, newLight);
                createDirectionalLightArrow(entity, transform, light, isSelected);
            }else if (light.type == LightType::POINT){
                createOrUpdateLightIcon(entity, transform, LightType::POINT, newLight);
                createPointLightSphere(entity, transform, light, isSelected);
            }else if (light.type == LightType::SPOT){
                createOrUpdateLightIcon(entity, transform, LightType::SPOT, newLight);
                createSpotLightCones(entity, transform, light, isSelected);
            }

            if (displaySettings.hideLightIcons && lightObjects.find(entity) != lightObjects.end()) {
                lightObjects[entity].icon->setVisible(false);
            }
        }

        if (signature.test(scene->getComponentId<CameraComponent>()) && signature.test(scene->getComponentId<Transform>())) {
            if (entity != camera->getEntity()) {
                Transform& transform = scene->getComponent<Transform>(entity);
                CameraComponent& cameraComp = scene->getComponent<CameraComponent>(entity);

                currentIconCameras.insert(entity);
                bool newCamera = instanciateCameraObject(entity);
                createOrUpdateCameraIcon(entity, transform, newCamera);
                createCameraFrustum(entity, transform, cameraComp, true, mainCamera == entity);

                if (displaySettings.hideCameraView) {
                    cameraObjects[entity].icon->setVisible(false);
                    cameraObjects[entity].lines->setVisible(false);
                }
            }
        }

        if (signature.test(scene->getComponentId<AudioSourceComponent>()) && signature.test(scene->getComponentId<Transform>())) {
            Transform& transform = scene->getComponent<Transform>(entity);

            currentIconSounds.insert(entity);
            bool newSound = instanciateSoundObject(entity);
            createOrUpdateSoundIcon(entity, transform, newSound);

            if (displaySettings.hideSoundIcons && soundObjects.find(entity) != soundObjects.end()) {
                soundObjects[entity].icon->setVisible(false);
            }
        }

        if (signature.test(scene->getComponentId<RigidbodyComponent>()) && signature.test(scene->getComponentId<Transform>())) {
            Transform& transform = scene->getComponent<Transform>(entity);
            RigidbodyComponent& body = scene->getComponent<RigidbodyComponent>(entity);

            currentBodyObjects.insert(entity);
            instanciateBodyObject(entity);
            bool highlighted = isDescendantSelected(entity);
            bool isVisible = displaySettings.showAllBodies || highlighted;

            createOrUpdateBodyLines(entity, transform, body, isVisible, highlighted);
        }

        if (signature.test(scene->getComponentId<LineRendererComponent>()) && signature.test(scene->getComponentId<Transform>())) {
            LineRendererComponent& linesComp = scene->getComponent<LineRendererComponent>(entity);
            Transform& transform = scene->getComponent<Transform>(entity);

            currentLinePoints.insert(entity);
            ensureOverlayLines(linePointLines, entity);
            bool highlighted = isDescendantSelected(entity);

            createOrUpdateLinePointLines(entity, transform, linesComp, highlighted);
        }

        if (signature.test(scene->getComponentId<PolygonComponent>()) && signature.test(scene->getComponentId<Transform>())) {
            PolygonComponent& polygon = scene->getComponent<PolygonComponent>(entity);
            Transform& transform = scene->getComponent<Transform>(entity);

            currentPolygonPoints.insert(entity);
            ensureOverlayLines(polygonPointLines, entity);
            bool highlighted = isDescendantSelected(entity);

            createOrUpdatePolygonPointLines(entity, transform, polygon.points, false, highlighted);
        }else if (signature.test(scene->getComponentId<MeshPolygonComponent>()) && signature.test(scene->getComponentId<Transform>())) {
            MeshPolygonComponent& polygon = scene->getComponent<MeshPolygonComponent>(entity);
            Transform& transform = scene->getComponent<Transform>(entity);

            currentPolygonPoints.insert(entity);
            ensureOverlayLines(polygonPointLines, entity);
            bool highlighted = isDescendantSelected(entity);

            createOrUpdatePolygonPointLines(entity, transform, polygon.points, true, highlighted);
        }

        if (signature.test(scene->getComponentId<TranslateTracksComponent>())) {
            TranslateTracksComponent& tracks = scene->getComponent<TranslateTracksComponent>(entity);

            currentTrackLines.insert(entity);
            instanciateTrackLines(entity);
            // the tracks entity has no Transform, so gate on direct selection
            bool isSelected = selectedEntities.find(entity) != selectedEntities.end();

            createOrUpdateTrackLines(entity, tracks, isSelected);
        }

        if (signature.test(scene->getComponentId<ReflectionProbeComponent>()) && signature.test(scene->getComponentId<Transform>())) {
            ReflectionProbeComponent& probe = scene->getComponent<ReflectionProbeComponent>(entity);
            Transform& transform = scene->getComponent<Transform>(entity);
            currentReflectionProbes.insert(entity);
            ensureOverlayLines(reflectionProbeLines, entity);
            createOrUpdateReflectionProbeLines(entity, transform, probe, selectedEntities.find(entity) != selectedEntities.end());
        }

        if (signature.test(scene->getComponentId<Joint3DComponent>())) {
            Joint3DComponent& joint = scene->getComponent<Joint3DComponent>(entity);

            currentJointObjects.insert(entity);
            ensureOverlayLines(jointLines, entity);

            bool isSelectedJoint = selectedEntities.find(entity) != selectedEntities.end();
            bool isBodyASelected = joint.bodyA != NULL_ENTITY && isDescendantSelected(joint.bodyA);
            bool isBodyBSelected = joint.bodyB != NULL_ENTITY && isDescendantSelected(joint.bodyB);
            bool highlighted = isSelectedJoint || isBodyASelected || isBodyBSelected;
            bool isVisible = displaySettings.showAllJoints || highlighted;

            createOrUpdateJointLines(entity, joint, isVisible, highlighted);
        }

        if (signature.test(scene->getComponentId<ModelComponent>())) {
            ModelComponent& model = scene->getComponent<ModelComponent>(entity);

            if (!model.bonesIdMapping.empty()) {
                currentBoneModels.insert(entity);
                instanciateBoneLines(entity);

                bool isSelected = isDescendantSelected(entity);
                bool isVisible = displaySettings.showAllBones || isSelected;

                createOrUpdateBoneLines(entity, model, isVisible, isSelected);
            }
        }
    }

    // Remove sun icons for entities that are no longer directional lights
    auto it = lightObjects.begin();
    while (it != lightObjects.end()) {
        if (currentIconLights.find(it->first) == currentIconLights.end()) {
            delete it->second.icon;
            delete it->second.lines;
            it = lightObjects.erase(it);
        } else {
            ++it;
        }
    }

    // Remove camera icons
    auto itCam = cameraObjects.begin();
    while (itCam != cameraObjects.end()) {
        if (currentIconCameras.find(itCam->first) == currentIconCameras.end()) {
            delete itCam->second.icon;
            delete itCam->second.lines;
            itCam = cameraObjects.erase(itCam);
        } else {
            ++itCam;
        }
    }

    auto itSound = soundObjects.begin();
    while (itSound != soundObjects.end()) {
        if (currentIconSounds.find(itSound->first) == currentIconSounds.end()) {
            delete itSound->second.icon;
            itSound = soundObjects.erase(itSound);
        } else {
            ++itSound;
        }
    }

    auto itBody = bodyObjects.begin();
    while (itBody != bodyObjects.end()) {
        if (currentBodyObjects.find(itBody->first) == currentBodyObjects.end()) {
            delete itBody->second.lines;
            itBody = bodyObjects.erase(itBody);
        } else {
            ++itBody;
        }
    }

    auto itJoint = jointLines.begin();
    while (itJoint != jointLines.end()) {
        if (currentJointObjects.find(itJoint->first) == currentJointObjects.end()) {
            delete itJoint->second;
            itJoint = jointLines.erase(itJoint);
        } else {
            ++itJoint;
        }
    }

    auto itBone = boneLines.begin();
    while (itBone != boneLines.end()) {
        if (currentBoneModels.find(itBone->first) == currentBoneModels.end()) {
            delete itBone->second;
            itBone = boneLines.erase(itBone);
        } else {
            ++itBone;
        }
    }

    auto itLinePoints = linePointLines.begin();
    while (itLinePoints != linePointLines.end()) {
        if (currentLinePoints.find(itLinePoints->first) == currentLinePoints.end()) {
            delete itLinePoints->second;
            itLinePoints = linePointLines.erase(itLinePoints);
        } else {
            ++itLinePoints;
        }
    }

    auto itPolygonPoints = polygonPointLines.begin();
    while (itPolygonPoints != polygonPointLines.end()) {
        if (currentPolygonPoints.find(itPolygonPoints->first) == currentPolygonPoints.end()) {
            delete itPolygonPoints->second;
            itPolygonPoints = polygonPointLines.erase(itPolygonPoints);
        } else {
            ++itPolygonPoints;
        }
    }

    auto itTrackLines = trackLines.begin();
    while (itTrackLines != trackLines.end()) {
        if (currentTrackLines.find(itTrackLines->first) == currentTrackLines.end()) {
            delete itTrackLines->second;
            itTrackLines = trackLines.erase(itTrackLines);
        } else {
            ++itTrackLines;
        }
    }

    auto itProbeLines = reflectionProbeLines.begin();
    while (itProbeLines != reflectionProbeLines.end()) {
        if (currentReflectionProbes.find(itProbeLines->first) == currentReflectionProbes.end()) {
            delete itProbeLines->second;
            itProbeLines = reflectionProbeLines.erase(itProbeLines);
        } else {
            ++itProbeLines;
        }
    }
}

editor::ViewportGizmo* editor::SceneRender3D::getViewportGizmo(){
    return &viewgizmo;
}
