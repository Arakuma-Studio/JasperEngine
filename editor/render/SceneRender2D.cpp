// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SceneRender2D.h"
#include "component/AudioSourceComponent.h"
#include "component/ButtonComponent.h"
#include "component/RectTransformComponent.h"
#include "component/Transform.h"
#include "subsystem/UISystem.h"
#include "AppSettings.h"
#include "component/LayoutGroupComponent.h"
#include "component/MeshPolygonComponent.h"
#include "component/OnScreenControlComponents.h"
#include "component/TranslateTracksComponent.h"
#include "component/UIDocumentComponent.h"
#include "object/Sprite.h"
#include "subsystem/RenderSystem.h"
#include "component/CharacterController2DComponent.h"

#include "Project.h"
#include "LineDrawUtils.h"
#include "Joint2DAnchors.h"
#include "physics/PolygonDecomposition2D.h"
#include "subsystem/PhysicsSystem.h"

#include <algorithm>
#include <cmath>


using namespace jasperengine;

editor::SceneRender2D::SceneRender2D(Scene* scene, unsigned int width, unsigned int height, bool isUI): SceneRender(scene, true, false, 40, 2){
    this->isUI = isUI;

    if (isUI){
        camera->setType(CameraType::CAMERA_UI);
    }else{
        camera->setType(CameraType::CAMERA_ORTHO);
    }
    camera->setNearClip(-10000.0f);
    camera->setFarClip(10000.0f);
    camera->setTransparentSort(false);
    camera->setDepthTest(false);

    camera->slide(-50);
    camera->slideUp(-50);

    scene->setDefaultEntityPool(EntityPool::System);
    lines = overlay(new LineRenderer(scene));
    gridLines = overlay(new LineRenderer(scene));
    gridLines->addLine(Vector3::ZERO, Vector3::ZERO, Vector4::ZERO);
    gridLines->setVisible(false);
    tileLines = overlay(new LineRenderer(scene));
    tileLines->addLine(Vector3::ZERO, Vector3::ZERO, Vector4::ZERO);
    tileLines->setVisible(false);
    documentLines = overlay(new LineRenderer(scene));
    documentLines->addLine(Vector3::ZERO, Vector3::ZERO, Vector4::ZERO);
    documentLines->setVisible(false);
    navigationLines = overlay(new LineRenderer(scene));
    navigationLines->addLine(Vector3::ZERO, Vector3::ZERO, Vector4::ZERO);
    navigationLines->setVisible(false);
    scene->setDefaultEntityPool(EntityPool::User);

    createLines(width, height);

    viewportWidth = width;
    viewportHeight = height;
    applyZoomProjection();
}

editor::SceneRender2D::~SceneRender2D(){
    // a gizmo the scene destroyed is not deleted through an id that may be another entity's now
    dropGoneGizmos();

    const RenderSystem& render = *scene->getSystem<RenderSystem>();
    deleteGizmos(icons, render);
    for (GizmoLineBatch* batch : lineBatches()) {
        deleteGizmos(*batch, render);
    }

    delete gridLines;
    delete tileLines;
    delete documentLines;
    delete navigationLines;
    // and the selection outline and the lines, as SceneRender3D's destructor does (leaked before)
    delete lines;
    delete selLines;
}

// square marker for an editable point (occluder polygon points, line endpoints)
void editor::SceneRender2D::addPointHandle(GizmoLineBatch* linesObj, const Vector3& worldPoint, float halfSize, const Vector4& color){
    linesObj->addLine(worldPoint + Vector3(-halfSize, -halfSize, 0.0f), worldPoint + Vector3(halfSize, -halfSize, 0.0f), color);
    linesObj->addLine(worldPoint + Vector3(halfSize, -halfSize, 0.0f), worldPoint + Vector3(halfSize, halfSize, 0.0f), color);
    linesObj->addLine(worldPoint + Vector3(halfSize, halfSize, 0.0f), worldPoint + Vector3(-halfSize, halfSize, 0.0f), color);
    linesObj->addLine(worldPoint + Vector3(-halfSize, halfSize, 0.0f), worldPoint + Vector3(-halfSize, -halfSize, 0.0f), color);
}

void editor::SceneRender2D::createOrUpdateLight2DLines(Entity entity, const Transform& transform, const Light2DComponent& light, bool visible, bool highlighted){
    GizmoLineBatch* lightLinesObj = &light2DLines;

    if (!transform.visible || !visible){
        return;
    }

    float alpha = highlighted ? 1.0f : 0.35f;
    const Vector4 lightColor(1.0f, 0.85f, 0.3f, alpha);
    const Vector3 center = transform.worldPosition;

    // range circle
    LineDrawUtils::addCircle2D(lightLinesObj, center, light.range, lightColor, 48);

    // small center cross
    float markSize = overlayPx(6.0f) * zoom;
    lightLinesObj->addLine(center + Vector3(-markSize, 0.0f, 0.0f), center + Vector3(markSize, 0.0f, 0.0f), lightColor);
    lightLinesObj->addLine(center + Vector3(0.0f, -markSize, 0.0f), center + Vector3(0.0f, markSize, 0.0f), lightColor);
}

// the circles the knob travels in (handle range) and reads 0 in (dead zone), on the base
void editor::SceneRender2D::createOrUpdateStickLines(Entity entity, const OnScreenStickComponent& stick, bool visible){
    GizmoLineBatch* linesObj = &stickLines;

    const Transform* transform = scene->findComponent<Transform>(stick.background);
    const RectTransformComponent* layout = scene->findComponent<RectTransformComponent>(stick.background);
    const bool shown = visible && transform && layout;
    if (!shown){
        return;
    }

    const Vector3 center = transform->modelMatrix * Vector3(layout->width / 2.0f, layout->height / 2.0f, 0.0f);
    const Vector3 edge = transform->modelMatrix * Vector3(static_cast<float>(layout->width), layout->height / 2.0f, 0.0f);
    const float radius = (edge - center).length();
    if (radius <= 0.0f){
        return;
    }
    LineDrawUtils::addCircle2D(linesObj, center, radius * stick.handleRange, Vector4(0.3f, 0.8f, 1.0f, 0.9f), 48);
    if (stick.deadZone > 0.0f){
        LineDrawUtils::addCircle2D(linesObj, center, radius * stick.deadZone, Vector4(1.0f, 0.5f, 0.3f, 0.9f), 32);
    }
    if (stick.mode == OnScreenStickMode::DYNAMIC){
        LineDrawUtils::addCircle2D(linesObj, center, radius * stick.moveThreshold, Vector4(0.3f, 0.8f, 1.0f, 0.4f), 48);
    }
}

void editor::SceneRender2D::createOrUpdateShadowCaster2DLines(Entity entity, const Transform& transform, const ShadowCaster2DComponent& occluder, bool visible, bool highlighted){
    GizmoLineBatch* occluderLinesObj = &shadowCaster2DLines;

    if (!transform.visible || !visible || !occluder.enabled){
        return;
    }

    float alpha = highlighted ? 1.0f : 0.35f;
    const Vector4 occluderColor(0.9f, 0.35f, 0.9f, alpha);
    const Vector4 handleColor(1.0f, 1.0f, 1.0f, alpha);
    const Matrix4& modelMatrix = transform.modelMatrix;

    // same outline the shadow pass builds: polygon points or the sibling mesh AABB
    std::vector<Vector2> points;
    bool closed = true;

    if (occluder.shape == ShadowCaster2DShape::AUTO_QUAD){
        MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        if (!mesh || mesh->aabb == AABB::ZERO){
            return;
        }
        Vector3 mn = mesh->aabb.getMinimum();
        Vector3 mx = mesh->aabb.getMaximum();
        points.push_back(Vector2(mn.x, mn.y));
        points.push_back(Vector2(mx.x, mn.y));
        points.push_back(Vector2(mx.x, mx.y));
        points.push_back(Vector2(mn.x, mx.y));
    }else{
        points = occluder.points;
        closed = occluder.closed;
    }

    if (points.size() < 2){
        return;
    }

    std::vector<Vector3> worldPoints;
    worldPoints.reserve(points.size());
    for (const Vector2& point : points){
        worldPoints.push_back(modelMatrix * Vector3(point.x, point.y, 0.0f));
    }

    size_t numSegments = closed ? worldPoints.size() : (worldPoints.size() - 1);
    for (size_t s = 0; s < numSegments; s++){
        occluderLinesObj->addLine(worldPoints[s], worldPoints[(s + 1) % worldPoints.size()], occluderColor);
    }

    // point handles (visual markers for the editable polygon points); the
    // sub-selected point is drawn bigger and in the selection color
    if (highlighted && occluder.shape == ShadowCaster2DShape::POLYGON){
        const Vector4 selectedHandleColor(1.0f, 0.6f, 0.0f, 1.0f);
        bool hasPointSelection = (getSelectedOccluderPointEntity() == entity);

        for (size_t i = 0; i < worldPoints.size(); i++){
            bool pointSelected = hasPointSelection && ((int)i == getSelectedOccluderPointIndex());
            addPointHandle(occluderLinesObj, worldPoints[i],
                           overlayPx(pointSelected ? 6.0f : 4.0f) * zoom,
                           pointSelected ? selectedHandleColor : handleColor);
        }
    }
}

// endpoint handles for a selected Lines entity (the lines themselves are the
// entity's own rendered geometry); the sub-selected endpoint is drawn bigger
// and in the selection color
void editor::SceneRender2D::createOrUpdateLinePointLines(Entity entity, const Transform& transform, const LineRendererComponent& lines, bool visible){
    GizmoLineBatch* handlesObj = &linePointLines;

    if (!transform.visible || !visible){
        return;
    }

    const Vector4 handleColor(1.0f, 1.0f, 1.0f, 1.0f);
    const Vector4 selectedHandleColor(1.0f, 0.6f, 0.0f, 1.0f);
    bool hasPointSelection = (getSelectedLinePointEntity() == entity);

    // a polyline's points, or each segment's ends
    const std::vector<Vector3> points = getLinePointWorldPositions(lines, transform);
    for (size_t i = 0; i < points.size(); i++){
        bool pointSelected = hasPointSelection && ((int)i == getSelectedLinePointIndex());
        addPointHandle(handlesObj, points[i],
                       overlayPx(pointSelected ? 6.0f : 4.0f) * zoom,
                       pointSelected ? selectedHandleColor : handleColor);
    }
}

// path of a selected TranslateTracks entity: a polyline through its keyframe
// values (in the action target's parent space) with square point handles; the
// sub-selected point is drawn bigger and in the selection color
void editor::SceneRender2D::createOrUpdateTrackLines(Entity entity, const TranslateTracksComponent& tracks, bool visible){
    GizmoLineBatch* trackObj = &trackLines;

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
        addPointHandle(trackObj, worldPoint,
                       overlayPx(pointSelected ? 6.0f : 4.0f) * zoom,
                       pointSelected ? selectedHandleColor : handleColor);
    }
}

// vertex handles for a selected Polygon / MeshPolygon entity; the sub-selected
// vertex is drawn bigger and in the selection color
void editor::SceneRender2D::createOrUpdatePolygonPointLines(Entity entity, const Transform& transform, const std::vector<PolygonPoint>& points, bool isMesh, bool visible){
    GizmoLineBatch* handlesObj = &polygonPointLines;

    if (!transform.visible || !visible){
        return;
    }

    const Vector4 handleColor(1.0f, 1.0f, 1.0f, 1.0f);
    const Vector4 selectedHandleColor(1.0f, 0.6f, 0.0f, 1.0f);
    bool hasPointSelection = (getSelectedPolygonPointEntity() == entity && isSelectedPolygonPointMesh() == isMesh);

    for (size_t i = 0; i < points.size(); i++){
        Vector3 worldPoint = transform.modelMatrix * points[i].position;

        bool pointSelected = hasPointSelection && ((int)i == getSelectedPolygonPointIndex());
        addPointHandle(handlesObj, worldPoint,
                       overlayPx(pointSelected ? 6.0f : 4.0f) * zoom,
                       pointSelected ? selectedHandleColor : handleColor);
    }
}

void editor::SceneRender2D::createOrUpdateBodyLines(Entity entity, const Transform& transform, Rigidbody2DComponent& body, bool visible, bool highlighted){
    GizmoLineBatch* bodyLinesObj = &bodyLines;

    const bool hasGenerated = body.generated && !body.generated->chunks.empty();
    if (!transform.visible || !visible || (body.colliders.empty() && !hasGenerated)){
        return;
    }

    float alpha = highlighted ? 1.0f : 0.35f;
    const Vector4 solidColor(0.2f, 0.95f, 0.95f, alpha);
    const Vector4 sensorColor(0.95f, 0.75f, 0.2f, alpha);
    Vector4 bodyColor = solidColor;
    // must match how PhysicsSystem sizes the Box2D shape, parent scale included
    const Vector2 entityScale(transform.worldScale.x, transform.worldScale.y);
    const Vector2 absScale(std::fabs(entityScale.x), std::fabs(entityScale.y));
    const float radiusScale = std::max(absScale.x, absScale.y);
    const Matrix4 bodyMatrix = Matrix4::translateMatrix(transform.worldPosition) * transform.worldRotation.getRotationMatrix();

    auto scaledPoint = [&](const Vector2& point){
        return point * entityScale;
    };

    auto scaledRadius = [&](float radius){
        return radius * radiusScale;
    };

    auto toWorld = [&](const Vector2& point){
        return bodyMatrix * Vector3(point.x, point.y, 0.0f);
    };

    auto addCircle = [&](const Vector2& center, float radius, int segments){
        if (radius <= 0.0f){
            return;
        }

        for (int i = 0; i < segments; i++){
            float a0 = (2.0f * M_PI * i) / segments;
            float a1 = (2.0f * M_PI * (i + 1)) / segments;

            Vector2 p0 = center + Vector2(std::cos(a0), std::sin(a0)) * radius;
            Vector2 p1 = center + Vector2(std::cos(a1), std::sin(a1)) * radius;

            bodyLinesObj->addLine(toWorld(p0), toWorld(p1), bodyColor);
        }
    };

    auto addArc = [&](const Vector2& center, float radius, float startAngle, float endAngle, int segments){
        if (radius <= 0.0f){
            return;
        }

        for (int i = 0; i < segments; i++){
            float t0 = (float)i / (float)segments;
            float t1 = (float)(i + 1) / (float)segments;
            float a0 = startAngle + (endAngle - startAngle) * t0;
            float a1 = startAngle + (endAngle - startAngle) * t1;

            Vector2 p0 = center + Vector2(std::cos(a0), std::sin(a0)) * radius;
            Vector2 p1 = center + Vector2(std::cos(a1), std::sin(a1)) * radius;

            bodyLinesObj->addLine(toWorld(p0), toWorld(p1), bodyColor);
        }
    };

    auto addRoundedRect = [&](const Vector2& a, const Vector2& b, float radius){
        Vector2 minPt(std::min(a.x, b.x), std::min(a.y, b.y));
        Vector2 maxPt(std::max(a.x, b.x), std::max(a.y, b.y));

        const float width = maxPt.x - minPt.x;
        const float height = maxPt.y - minPt.y;
        if (width <= 0.0f || height <= 0.0f){
            return;
        }

        const float clampedRadius = std::max(0.0f, std::min(radius, std::min(width, height) * 0.5f));
        if (clampedRadius <= 0.0f){
            Vector2 p0(minPt.x, minPt.y);
            Vector2 p1(maxPt.x, minPt.y);
            Vector2 p2(maxPt.x, maxPt.y);
            Vector2 p3(minPt.x, maxPt.y);

            bodyLinesObj->addLine(toWorld(p0), toWorld(p1), bodyColor);
            bodyLinesObj->addLine(toWorld(p1), toWorld(p2), bodyColor);
            bodyLinesObj->addLine(toWorld(p2), toWorld(p3), bodyColor);
            bodyLinesObj->addLine(toWorld(p3), toWorld(p0), bodyColor);
            return;
        }

        const float x0 = minPt.x;
        const float y0 = minPt.y;
        const float x1 = maxPt.x;
        const float y1 = maxPt.y;
        const float r = clampedRadius;

        bodyLinesObj->addLine(toWorld(Vector2(x0 + r, y0)), toWorld(Vector2(x1 - r, y0)), bodyColor);
        bodyLinesObj->addLine(toWorld(Vector2(x1, y0 + r)), toWorld(Vector2(x1, y1 - r)), bodyColor);
        bodyLinesObj->addLine(toWorld(Vector2(x1 - r, y1)), toWorld(Vector2(x0 + r, y1)), bodyColor);
        bodyLinesObj->addLine(toWorld(Vector2(x0, y1 - r)), toWorld(Vector2(x0, y0 + r)), bodyColor);

        addArc(Vector2(x0 + r, y0 + r), r, M_PI, 1.5f * M_PI, 8);
        addArc(Vector2(x1 - r, y0 + r), r, 1.5f * M_PI, 2.0f * M_PI, 8);
        addArc(Vector2(x1 - r, y1 - r), r, 0.0f, 0.5f * M_PI, 8);
        addArc(Vector2(x0 + r, y1 - r), r, 0.5f * M_PI, M_PI, 8);
    };

    if (hasGenerated){
        for (const auto& [coord, chunk] : body.generated->chunks){
            for (const Collider2D& shape : chunk.colliders){
                const Vector4 color = shape.sensor ? sensorColor : solidColor;
                const size_t count = shape.points.size();
                const bool closed = shape.loop || shape.type == Collider2DType::POLYGON;
                // an open chain's first and last edges are ghosts: its neighbouring chunk draws them
                const size_t end = closed ? count : (count > 2 ? count - 2 : 0);
                for (size_t j = closed ? 0 : 1; j < end; j++){
                    bodyLinesObj->addLine(toWorld(scaledPoint(shape.points[j])), toWorld(scaledPoint(shape.points[(j + 1) % count])), color);
                }
            }
        }
    }

    for (size_t i = 0; i < body.colliders.size(); i++){
        const Collider2D& shape = body.colliders[i];
        bodyColor = shape.sensor ? sensorColor : solidColor;

        auto addPolygonEdges = [&](){
            const size_t count = shape.points.size();
            for (size_t j = 0; j < count; j++){
                Vector2 p0 = scaledPoint(shape.points[j]);
                Vector2 p1 = scaledPoint(shape.points[(j + 1) % count]);
                bodyLinesObj->addLine(toWorld(p0), toWorld(p1), bodyColor);
            }
            // a selected concave polygon shows the convex pieces Box2D gets, faintly
            if (highlighted && count > 3){
                // the body's own pieces (cached on the collider by points and scale)
                const std::vector<std::vector<Vector2>> pieces = scene->getSystem<PhysicsSystem>()->getCollider2DPiecesLocal(entity, body.colliders[i]);
                if (pieces.size() > 1){
                    const Vector4 seamColor(bodyColor.x, bodyColor.y, bodyColor.z, bodyColor.w * 0.3f);
                    for (const std::vector<Vector2>& piece : pieces){
                        for (size_t k = 0; k < piece.size(); k++){
                            bodyLinesObj->addLine(toWorld(scaledPoint(piece[k])), toWorld(scaledPoint(piece[(k + 1) % piece.size()])), seamColor);
                        }
                    }
                }
            }
        };

        if (shape.type == Collider2DType::POLYGON){
            if (shape.points.size() >= 3){
                if (shape.radius > 0.0f && shape.points.size() == 4){
                    Vector2 minPt = scaledPoint(shape.points[0]);
                    Vector2 maxPt = minPt;
                    for (size_t j = 1; j < shape.points.size(); j++){
                        Vector2 p = scaledPoint(shape.points[j]);
                        minPt.x = std::min(minPt.x, p.x);
                        minPt.y = std::min(minPt.y, p.y);
                        maxPt.x = std::max(maxPt.x, p.x);
                        maxPt.y = std::max(maxPt.y, p.y);
                    }
                    addRoundedRect(minPt, maxPt, scaledRadius(shape.radius));
                }else{
                    addPolygonEdges();
                }
            }else{
                Vector2 pointA = scaledPoint(shape.pointA);
                Vector2 pointB = scaledPoint(shape.pointB);
                Vector2 minPt(std::min(pointA.x, pointB.x), std::min(pointA.y, pointB.y));
                Vector2 maxPt(std::max(pointA.x, pointB.x), std::max(pointA.y, pointB.y));

                Vector2 p0(minPt.x, minPt.y);
                Vector2 p1(maxPt.x, minPt.y);
                Vector2 p2(maxPt.x, maxPt.y);
                Vector2 p3(minPt.x, maxPt.y);

                bodyLinesObj->addLine(toWorld(p0), toWorld(p1), bodyColor);
                bodyLinesObj->addLine(toWorld(p1), toWorld(p2), bodyColor);
                bodyLinesObj->addLine(toWorld(p2), toWorld(p3), bodyColor);
                bodyLinesObj->addLine(toWorld(p3), toWorld(p0), bodyColor);
            }
        }else if (shape.type == Collider2DType::CIRCLE){
            addCircle(scaledPoint(shape.pointA), scaledRadius(shape.radius), 24);
        }else if (shape.type == Collider2DType::CAPSULE){
            Vector2 pointA = scaledPoint(shape.pointA);
            Vector2 pointB = scaledPoint(shape.pointB);
            float radius = scaledRadius(shape.radius);

            if (radius <= 0.0f){
                continue;
            }

            Vector2 axis = pointB - pointA;
            if (axis.length() <= 0.0f){
                addCircle(pointA, radius, 24);
                continue;
            }

            axis.normalize();
            Vector2 perp(-axis.y, axis.x);

            Vector2 sideA1 = pointA + perp * radius;
            Vector2 sideB1 = pointB + perp * radius;
            Vector2 sideA2 = pointA - perp * radius;
            Vector2 sideB2 = pointB - perp * radius;

            bodyLinesObj->addLine(toWorld(sideA1), toWorld(sideB1), bodyColor);
            bodyLinesObj->addLine(toWorld(sideA2), toWorld(sideB2), bodyColor);

            float baseAngle = std::atan2(axis.y, axis.x);
            addArc(pointA, radius, baseAngle + 0.5f * M_PI, baseAngle + 1.5f * M_PI, 12);
            addArc(pointB, radius, baseAngle - 0.5f * M_PI, baseAngle + 0.5f * M_PI, 12);
        }else if (shape.type == Collider2DType::SEGMENT){
            bodyLinesObj->addLine(toWorld(scaledPoint(shape.pointA)), toWorld(scaledPoint(shape.pointB)), bodyColor);
        }else if (shape.type == Collider2DType::CHAIN){
            if (shape.points.size() >= 2){
                for (size_t j = 0; j < shape.points.size() - 1; j++){
                    bodyLinesObj->addLine(toWorld(scaledPoint(shape.points[j])), toWorld(scaledPoint(shape.points[j + 1])), bodyColor);
                }

                if (shape.loop && shape.points.size() >= 3){
                    bodyLinesObj->addLine(toWorld(scaledPoint(shape.points[shape.points.size() - 1])), toWorld(scaledPoint(shape.points[0])), bodyColor);
                }
            }

            // ticks on the colliding side (right of each edge), open chains skip the end edges;
            // a chain of 2 or 3 points is two-sided segments
            if (shape.points.size() >= 4){
                size_t n = shape.points.size();
                size_t first = shape.loop ? 0 : 1;
                size_t end = shape.loop ? n : n - 2;
                // mirrored chains are reversed in PhysicsSystem, keeping the side
                float side = (entityScale.x * entityScale.y < 0.0f) ? -1.0f : 1.0f;

                for (size_t j = first; j < end; j++){
                    Vector2 p0 = scaledPoint(shape.points[j]);
                    Vector2 p1 = scaledPoint(shape.points[(j + 1) % n]);
                    Vector2 dir = p1 - p0;
                    float length = dir.normalizeL();
                    if (length > 0.0f){
                        Vector2 mid = p0.midPoint(p1);
                        Vector2 normal(dir.y * side, -dir.x * side);
                        bodyLinesObj->addLine(toWorld(mid), toWorld(mid + normal * std::min(overlayPx(8.0f) * zoom, length * 0.5f)), bodyColor);
                    }
                }
            }
        }
    }
}

// the upright capsule the CharacterController2DSystem moves: as it sizes it, the world scale
// (radius by x, height by y) and no rotation
void editor::SceneRender2D::createOrUpdateController2DLines(Entity entity, const Transform& transform, const CharacterController2DComponent& controller, bool visible, bool highlighted){
    GizmoLineBatch* linesObj = &controllerLines;
    if (!transform.visible || !visible){
        return;
    }

    const Vector2 scale(std::fabs(transform.worldScale.x), std::fabs(transform.worldScale.y));
    const float radius = std::max(0.0f, controller.radius) * scale.x;
    const float height = std::max(0.0f, controller.height) * scale.y;
    if (radius <= 0.0f){
        return;
    }
    const float halfSegment = std::max(0.0f, height * 0.5f - radius);
    const Vector2 center = Vector2(transform.worldPosition.x, transform.worldPosition.y) + controller.center * scale;
    const float z = transform.worldPosition.z;
    const Vector4 color = controller.enabled ? Vector4(0.45f, 0.95f, 0.35f, highlighted ? 1.0f : 0.35f)
                                             : Vector4(0.6f, 0.6f, 0.6f, highlighted ? 0.8f : 0.3f);
    auto point = [&](const Vector2& p){ return Vector3(p.x, p.y, z); };
    const Vector2 top = center + Vector2(0.0f, halfSegment);
    const Vector2 bottom = center - Vector2(0.0f, halfSegment);
    linesObj->addLine(point(top + Vector2(-radius, 0.0f)), point(bottom + Vector2(-radius, 0.0f)), color);
    linesObj->addLine(point(top + Vector2(radius, 0.0f)), point(bottom + Vector2(radius, 0.0f)), color);
    constexpr int segments = 12;
    for (int i = 0; i < segments; i++){
        const float a0 = (float)M_PI * i / segments;
        const float a1 = (float)M_PI * (i + 1) / segments;
        linesObj->addLine(point(top + Vector2(std::cos(a0), std::sin(a0)) * radius), point(top + Vector2(std::cos(a1), std::sin(a1)) * radius), color);
        linesObj->addLine(point(bottom - Vector2(std::cos(a0), std::sin(a0)) * radius), point(bottom - Vector2(std::cos(a1), std::sin(a1)) * radius), color);
    }
    // the step it climbs, over the feet
    if (controller.stepOffset > 0.0f){
        const float feet = bottom.y - radius;
        linesObj->addLine(point(Vector2(center.x - radius, feet + controller.stepOffset)), point(Vector2(center.x + radius, feet + controller.stepOffset)),
                          Vector4(color.x, color.y, color.z, color.w * 0.5f));
    }
}

void editor::SceneRender2D::createOrUpdateJointLines(Entity entity, const Joint2DComponent& joint, bool visible, bool highlighted){
    GizmoLineBatch* jointLinesObj = &jointLines;

    if (!visible){
        return;
    }

    float alpha = highlighted ? 1.0f : 0.35f;

    const Vector4 jointColor(0.95f, 0.75f, 0.2f, alpha);
    const Vector4 helperColor(0.85f, 0.85f, 0.85f, 0.8f * alpha);
    const Vector4 axisColor(0.2f, 0.9f, 1.0f, alpha);
    const Vector4 limitColor(1.0f, 0.35f, 0.35f, alpha);

    // where PhysicsSystem puts the anchors (local to each body, or world points)
    const Joint2DAnchorsWorld anchors = joint2DAnchorsWorld(scene, joint);
    const Vector3 anchorA = anchors.anchorA;
    const Vector3 anchorB = anchors.anchorB;

    jointLinesObj->addLine(anchorA, anchorB, jointColor);

    const float sz = overlayScale * zoom;
    float markSize = 8.0f * sz;

    // anchor A: a circle and cross; anchor B: a smaller circle (the handles the Move tool drags)
    LineDrawUtils::addCircle2D(jointLinesObj, anchorA, 5.0f * sz, jointColor, 12);
    jointLinesObj->addLine(anchorA + Vector3(-markSize, 0.0f, 0.0f), anchorA + Vector3(markSize, 0.0f, 0.0f), helperColor);
    jointLinesObj->addLine(anchorA + Vector3(0.0f, -markSize, 0.0f), anchorA + Vector3(0.0f, markSize, 0.0f), helperColor);

    if ((anchorB - anchorA).length() > 0.001f){
        LineDrawUtils::addCircle2D(jointLinesObj, anchorB, 4.0f * sz, anchors.anchorBEditable ? jointColor : helperColor, 12);
        jointLinesObj->addLine(anchorB + Vector3(-markSize, 0.0f, 0.0f), anchorB + Vector3(markSize, 0.0f, 0.0f), helperColor);
        jointLinesObj->addLine(anchorB + Vector3(0.0f, -markSize, 0.0f), anchorB + Vector3(0.0f, markSize, 0.0f), helperColor);
    }

    auto addTick = [&](const Vector3& at, const Vector3& direction, float halfLength, const Vector4& color){
        const Vector3 perp(-direction.y, direction.x, 0.0f);
        jointLinesObj->addLine(at - perp * halfLength, at + perp * halfLength, color);
    };

    switch (joint.type){
        case Joint2DType::DISTANCE:{
            // the length kept (or its limits): ticks along the line from anchor A
            Vector3 direction = anchorB - anchorA;
            direction.z = 0.0f;
            const float current = direction.length();
            if (current > 0.001f){
                direction = direction * (1.0f / current);
                if (!joint.autoLength && joint.length > 0.0f){
                    addTick(anchorA + direction * joint.length, direction, 6.0f * sz, axisColor);
                }
                if (joint.useLimits || joint.rope){
                    const float length = joint.autoLength ? current : joint.length;
                    const float minLength = joint.useLimits ? joint.minLength : 0.0f;
                    const float maxLength = joint.rope ? length : (joint.maxLength > 0.0f ? joint.maxLength : length);
                    addTick(anchorA + direction * minLength, direction, 5.0f * sz, limitColor);
                    addTick(anchorA + direction * maxLength, direction, 5.0f * sz, limitColor);
                }
            }
            break;
        }
        case Joint2DType::REVOLUTE:{
            LineDrawUtils::addCircle2D(jointLinesObj, anchorA, 10.0f * sz, helperColor, 18);
            if (joint.useLimits){
                // the angle range, from bodyA's frame
                const float radius = 18.0f * sz;
                const float lower = anchors.bodyAAngle + std::max(-180.0f, std::min(joint.lowerAngle, joint.upperAngle)) * (float)M_PI / 180.0f;
                const float upper = anchors.bodyAAngle + std::min(180.0f, std::max(joint.lowerAngle, joint.upperAngle)) * (float)M_PI / 180.0f;
                LineDrawUtils::addArc2D(jointLinesObj, anchorA, radius, lower, upper, limitColor, 20);
                jointLinesObj->addLine(anchorA, anchorA + Vector3(std::cos(lower), std::sin(lower), 0.0f) * radius, limitColor);
                jointLinesObj->addLine(anchorA, anchorA + Vector3(std::cos(upper), std::sin(upper), 0.0f) * radius, limitColor);
            }
            break;
        }
        case Joint2DType::WHEEL:
        case Joint2DType::PRISMATIC:{
            const Vector3 axis3 = anchors.axis;
            const Vector3 railPerp(-axis3.y, axis3.x, 0.0f);
            if (joint.type == Joint2DType::WHEEL){
                LineDrawUtils::addCircle2D(jointLinesObj, anchorA, 10.0f * sz, helperColor, 18);
            }

            // the line of motion
            const float halfRail = 40.0f * sz;
            const float railOffset = (joint.type == Joint2DType::PRISMATIC ? 5.0f : 2.0f) * sz;
            Vector3 railStart = anchorA - axis3 * halfRail;
            Vector3 railEnd = anchorA + axis3 * halfRail;
            jointLinesObj->addLine(railStart + railPerp * railOffset, railEnd + railPerp * railOffset, axisColor);
            jointLinesObj->addLine(railStart - railPerp * railOffset, railEnd - railPerp * railOffset, axisColor);

            // the translation limits: a bar between them
            if (joint.useLimits){
                const float lower = std::min(joint.lowerTranslation, joint.upperTranslation);
                const float upper = std::max(joint.lowerTranslation, joint.upperTranslation);
                const Vector3 lowerPoint = anchorA + axis3 * lower;
                const Vector3 upperPoint = anchorA + axis3 * upper;
                jointLinesObj->addLine(lowerPoint, upperPoint, limitColor);
                addTick(lowerPoint, axis3, 6.0f * sz, limitColor);
                addTick(upperPoint, axis3, 6.0f * sz, limitColor);
            }

            // the axis arrow
            const Vector3 arrowEnd = anchorA + axis3 * (24.0f * sz);
            jointLinesObj->addLine(anchorA, arrowEnd, axisColor);
            jointLinesObj->addLine(arrowEnd, arrowEnd - axis3 * (5.0f * sz) + railPerp * (3.0f * sz), axisColor);
            jointLinesObj->addLine(arrowEnd, arrowEnd - axis3 * (5.0f * sz) - railPerp * (3.0f * sz), axisColor);

            if (joint.type == Joint2DType::PRISMATIC){
                float t = (anchorB - anchorA).x * axis3.x + (anchorB - anchorA).y * axis3.y;
                t = std::max(-halfRail, std::min(halfRail, t));
                LineDrawUtils::addCircle2D(jointLinesObj, anchorA + axis3 * t, 7.0f * sz, jointColor, 14);
            }
            break;
        }
        case Joint2DType::WELD:{
            // Diamond shape to indicate rigid lock
            float d = 10.0f * sz;
            jointLinesObj->addLine(anchorA + Vector3(0, d, 0), anchorA + Vector3(d, 0, 0), limitColor);
            jointLinesObj->addLine(anchorA + Vector3(d, 0, 0), anchorA + Vector3(0, -d, 0), limitColor);
            jointLinesObj->addLine(anchorA + Vector3(0, -d, 0), anchorA + Vector3(-d, 0, 0), limitColor);
            jointLinesObj->addLine(anchorA + Vector3(-d, 0, 0), anchorA + Vector3(0, d, 0), limitColor);
            break;
        }
        case Joint2DType::MOTOR:{
            // Arrow arc to indicate driven rotation
            float radius = 10.0f * sz;
            float startAngle = 0.0f;
            float endAngle = 1.5f * M_PI;
            LineDrawUtils::addArc2D(jointLinesObj, anchorA, radius, startAngle, endAngle, axisColor, 14);
            // Arrowhead at end of arc
            float tipX = anchorA.x + std::cos(endAngle) * radius;
            float tipY = anchorA.y + std::sin(endAngle) * radius;
            Vector3 tip(tipX, tipY, anchorA.z);
            float arrowSize = 4.0f * sz;
            Vector3 dir(std::cos(endAngle + M_PI * 0.5f), std::sin(endAngle + M_PI * 0.5f), 0.0f);
            Vector3 perp(-dir.y, dir.x, 0.0f);
            jointLinesObj->addLine(tip, tip - dir * arrowSize + perp * arrowSize * 0.5f, axisColor);
            jointLinesObj->addLine(tip, tip - dir * arrowSize - perp * arrowSize * 0.5f, axisColor);
            break;
        }
        case Joint2DType::MOUSE:{
            LineDrawUtils::addCircle2D(jointLinesObj, anchorB, 8.0f * sz, Vector4(1.0f, 0.4f, 0.4f, alpha), 16);
            jointLinesObj->addLine(anchorB + Vector3(-markSize, 0.0f, 0.0f), anchorB + Vector3(markSize, 0.0f, 0.0f), limitColor);
            jointLinesObj->addLine(anchorB + Vector3(0.0f, -markSize, 0.0f), anchorB + Vector3(0.0f, markSize, 0.0f), limitColor);
            break;
        }
    }
}

void editor::SceneRender2D::createLines(unsigned int width, unsigned int height){
    canvasFrameWidth = width;
    canvasFrameHeight = height;
    lines->clearLines();

    lines->addLine(Vector3(0, -1000000, 0), Vector3(0, 1000000, 0), Vector4(0.2, 0.8, 0.4, 1.0));
    lines->addLine(Vector3(-1000000, 0, 0), Vector3(1000000, 0, 0), Vector4(0.8, 0.2, 0.4, 1.0));

    lines->addLine(Vector3(0, height, 0), Vector3(width, height, 0), Vector4(0.8, 0.8, 0.8, 1.0));
    lines->addLine(Vector3(width, height, 0), Vector3(width, 0, 0), Vector4(0.8, 0.8, 0.8, 1.0));
}

void editor::SceneRender2D::updateGridLines(){
    gridLines->clearLines();

    if (!displaySettings.showGrid2D || displaySettings.gridSpacing2D <= 0.0f){
        gridLines->setVisible(false);
        return;
    }

    gridLines->setVisible(true);

    float spacing = displaySettings.gridSpacing2D;

    Vector3 camPos = camera->getWorldPosition();

    float left = camera->getLeftClip() + camPos.x;
    float right = camera->getRightClip() + camPos.x;
    float bottom = camera->getBottomClip() + camPos.y;
    float top = camera->getTopClip() + camPos.y;

    // Add margin to avoid popping at edges
    float margin = spacing * 2.0f;
    left -= margin;
    right += margin;
    bottom -= margin;
    top += margin;

    Vector4 gridColor(0.5f, 0.5f, 0.5f, 0.3f);
    Vector4 gridColorMajor(0.5f, 0.5f, 0.5f, 0.6f);
    int majorEvery = 5;

    // Vertical lines
    float startX = std::floor(left / spacing) * spacing;
    for (float x = startX; x <= right; x += spacing){
        int ix = (int)std::round(x / spacing);
        Vector4 color = (ix % majorEvery == 0) ? gridColorMajor : gridColor;
        gridLines->addLine(Vector3(x, bottom, 0), Vector3(x, top, 0), color);
    }

    // Horizontal lines
    float startY = std::floor(bottom / spacing) * spacing;
    for (float y = startY; y <= top; y += spacing){
        int iy = (int)std::round(y / spacing);
        Vector4 color = (iy % majorEvery == 0) ? gridColorMajor : gridColor;
        gridLines->addLine(Vector3(left, y, 0), Vector3(right, y, 0), color);
    }
}

std::vector<editor::GizmoLineBatch*> editor::SceneRender2D::lineBatches(){
    return {&containerLines, &bodyLines, &jointLines, &controllerLines, &light2DLines, &shadowCaster2DLines, &linePointLines,
            &polygonPointLines, &trackLines, &stickLines, &cameraLines};
}

void editor::SceneRender2D::dropGoneGizmos(){
    SceneRender::dropGoneGizmos();

    const RenderSystem& render = *scene->getSystem<RenderSystem>();
    dropGoneGizmos(icons, render);
    for (GizmoLineBatch* batch : lineBatches()) {
        dropGoneGizmos(*batch, render);
    }
}

void editor::SceneRender2D::hideAllGizmos(){
    SceneRender::hideAllGizmos();

    lines->setVisible(false);
    gridLines->setVisible(false);
    tileLines->setVisible(false);
    documentLines->setVisible(false);
    navigationLines->setVisible(false);
    icons.hide();
    for (GizmoLineBatch* batch : lineBatches()) {
        batch->hide();
    }
}

void editor::SceneRender2D::activate(){
    SceneRender::activate();
}

void editor::SceneRender2D::applyZoomProjection(){
    Entity cameraEntity = camera->getEntity();
    CameraComponent& cameracomp = scene->getComponent<CameraComponent>(cameraEntity);
    Transform& cameratransform = scene->getComponent<Transform>(cameraEntity);

    float newWidth = viewportWidth * zoom;
    float newHeight = viewportHeight * zoom;

    if (cameracomp.leftClip != 0.0f || cameracomp.bottomClip != 0.0f || cameracomp.rightClip != newWidth || cameracomp.topClip != newHeight){
        cameracomp.leftClip = 0.0f;
        cameracomp.bottomClip = 0.0f;
        cameracomp.rightClip = newWidth;
        cameracomp.topClip = newHeight;
        cameracomp.needUpdate = true;
    }

    cameracomp.autoResize = false;

    toolslayer.updateCamera(cameracomp, cameratransform);
}

void editor::SceneRender2D::setCanvasFrameSize(unsigned int width, unsigned int height){
    createLines(width, height);
}

void editor::SceneRender2D::updateSize(int width, int height){
    SceneRender::updateSize(width, height);

    viewportWidth = width;
    viewportHeight = height;
    applyZoomProjection();
}

void editor::SceneRender2D::updateSelLines(std::vector<OBB> obbs){
    fillSelectionLines(selLines, obbs, false);
}

void editor::SceneRender2D::update(std::vector<Entity> selEntities, std::vector<Entity> entities, Entity mainCamera, const SceneDisplaySettings& settings){
    SceneRender::update(selEntities, entities, mainCamera, settings);

    if (isPreviewCameraActive()){
        return;
    }

    lines->setVisible(displaySettings.showOrigin);

    updateGridLines();

    std::set<Entity> selectedEntities(selEntities.begin(), selEntities.end());

    auto isDescendantSelected = [&](Entity ancestor) -> bool {
        for (Entity sel : selectedEntities) {
            if (sel == ancestor || scene->isParentOf(ancestor, sel)) return true;
        }
        return false;
    };

    // every kind's batch is rebuilt from the entities each update; a batch is written to its
    // renderer only when what it draws changed
    icons.begin();
    particleShapeLines.begin();
    for (GizmoLineBatch* batch : lineBatches()) {
        batch->begin();
    }
    const float iconScale = 0.25f;
    for (Entity& entity: entities){
        Signature signature = scene->getSignature(entity);
        // Unity draws no gizmo for a GameObject inactive in hierarchy (the icon picker refuses it too)
        if (!gizmoActive(entity)){
            continue;
        }
        if (signature.test(scene->getComponentId<LayoutGroupComponent>()) && 
            signature.test(scene->getComponentId<Transform>()) && 
            signature.test(scene->getComponentId<RectTransformComponent>())){

            GizmoLineBatch* containerLinesObj = &containerLines;

            Transform& transform = scene->getComponent<Transform>(entity);
            RectTransformComponent& layout = scene->getComponent<RectTransformComponent>(entity);
            LayoutGroupComponent& container = scene->getComponent<LayoutGroupComponent>(entity);

            if (transform.visible && !displaySettings.hideContainerGuides){

                Vector4 borderColor(0.55f, 0.35f, 0.85f, 1.0f); // Purple color for container
                Vector4 divColor(0.55f, 0.35f, 0.85f, 0.5f); // Semi-transparent purple for divisions

                Matrix4 modelMatrix = transform.modelMatrix;

                // Container borders
                Vector3 p1 = modelMatrix * Vector3(0, 0, 0);
                Vector3 p2 = modelMatrix * Vector3(layout.width, 0, 0);
                Vector3 p3 = modelMatrix * Vector3(layout.width, layout.height, 0);
                Vector3 p4 = modelMatrix * Vector3(0, layout.height, 0);

                containerLinesObj->addLine(p1, p2, borderColor);
                containerLinesObj->addLine(p2, p3, borderColor);
                containerLinesObj->addLine(p3, p4, borderColor);
                containerLinesObj->addLine(p4, p1, borderColor);

                // Divisions
                for (int b = 0; b < container.numBoxes; b++){
                    if (container.boxes[b].layout != NULL_ENTITY){
                        Rect rect = container.boxes[b].rect;

                        Vector3 bp1 = modelMatrix * Vector3(rect.getX(), rect.getY(), 0);
                        Vector3 bp2 = modelMatrix * Vector3(rect.getX() + rect.getWidth(), rect.getY(), 0);
                        Vector3 bp3 = modelMatrix * Vector3(rect.getX() + rect.getWidth(), rect.getY() + rect.getHeight(), 0);
                        Vector3 bp4 = modelMatrix * Vector3(rect.getX(), rect.getY() + rect.getHeight(), 0);

                        containerLinesObj->addLine(bp1, bp2, divColor);
                        containerLinesObj->addLine(bp2, bp3, divColor);
                        containerLinesObj->addLine(bp3, bp4, divColor);
                        containerLinesObj->addLine(bp4, bp1, divColor);
                    }
                }
            }
        }

        if (signature.test(scene->getComponentId<Rigidbody2DComponent>()) && signature.test(scene->getComponentId<Transform>())){
            Rigidbody2DComponent& body = scene->getComponent<Rigidbody2DComponent>(entity);
            Transform& transform = scene->getComponent<Transform>(entity);

            bool highlighted = isDescendantSelected(entity);
            bool isVisible = displaySettings.showAllBodies || highlighted;

            createOrUpdateBodyLines(entity, transform, body, isVisible, highlighted);
        }

        if (signature.test(scene->getComponentId<CharacterController2DComponent>()) && signature.test(scene->getComponentId<Transform>())){
            const CharacterController2DComponent& controller = scene->getComponent<CharacterController2DComponent>(entity);
            Transform& transform = scene->getComponent<Transform>(entity);

            const bool highlighted = isDescendantSelected(entity);
            createOrUpdateController2DLines(entity, transform, controller, displaySettings.showAllBodies || highlighted, highlighted);
        }

        if (signature.test(scene->getComponentId<OnScreenStickComponent>())){
            createOrUpdateStickLines(entity, scene->getComponent<OnScreenStickComponent>(entity), isDescendantSelected(entity));
        }

        if (signature.test(scene->getComponentId<Light2DComponent>()) && signature.test(scene->getComponentId<Transform>())){
            Light2DComponent& light2d = scene->getComponent<Light2DComponent>(entity);
            Transform& transform = scene->getComponent<Transform>(entity);

            bool highlighted = isDescendantSelected(entity);

            createOrUpdateLight2DLines(entity, transform, light2d, highlighted, highlighted);

            if (transform.visible){
                icons.add(GizmoIcon::Bulb, transform.worldPosition, billboardScreenScale(transform.worldPosition, iconScale));
            }
        }

        if (signature.test(scene->getComponentId<ShadowCaster2DComponent>()) && signature.test(scene->getComponentId<Transform>())){
            ShadowCaster2DComponent& shadowcaster2d = scene->getComponent<ShadowCaster2DComponent>(entity);
            Transform& transform = scene->getComponent<Transform>(entity);

            bool highlighted = isDescendantSelected(entity);

            createOrUpdateShadowCaster2DLines(entity, transform, shadowcaster2d, highlighted, highlighted);
        }

        if (signature.test(scene->getComponentId<ParticleSystemComponent>())){
            updateParticleShapeLines(entity, selectedEntities.find(entity) != selectedEntities.end());
        }

        if (signature.test(scene->getComponentId<LineRendererComponent>()) && signature.test(scene->getComponentId<Transform>())){
            LineRendererComponent& linesComp = scene->getComponent<LineRendererComponent>(entity);
            Transform& transform = scene->getComponent<Transform>(entity);

            bool highlighted = isDescendantSelected(entity);

            createOrUpdateLinePointLines(entity, transform, linesComp, highlighted);
        }

        if (signature.test(scene->getComponentId<PolygonComponent>()) && signature.test(scene->getComponentId<Transform>())){
            PolygonComponent& polygon = scene->getComponent<PolygonComponent>(entity);
            Transform& transform = scene->getComponent<Transform>(entity);

            bool highlighted = isDescendantSelected(entity);

            createOrUpdatePolygonPointLines(entity, transform, polygon.points, false, highlighted);
        }else if (signature.test(scene->getComponentId<MeshPolygonComponent>()) && signature.test(scene->getComponentId<Transform>())){
            MeshPolygonComponent& polygon = scene->getComponent<MeshPolygonComponent>(entity);
            Transform& transform = scene->getComponent<Transform>(entity);

            bool highlighted = isDescendantSelected(entity);

            createOrUpdatePolygonPointLines(entity, transform, polygon.points, true, highlighted);
        }

        if (signature.test(scene->getComponentId<TranslateTracksComponent>())){
            TranslateTracksComponent& tracks = scene->getComponent<TranslateTracksComponent>(entity);

            // the tracks entity has no Transform, so gate on direct selection
            bool isSelected = selectedEntities.find(entity) != selectedEntities.end();

            createOrUpdateTrackLines(entity, tracks, isSelected);
        }

        if (signature.test(scene->getComponentId<Joint2DComponent>())){
            Joint2DComponent& joint = scene->getComponent<Joint2DComponent>(entity);


            bool isSelectedJoint = selectedEntities.find(entity) != selectedEntities.end();
            bool isBodyASelected = joint.bodyA != NULL_ENTITY && isDescendantSelected(joint.bodyA);
            bool isBodyBSelected = joint.bodyB != NULL_ENTITY && isDescendantSelected(joint.bodyB);
            bool highlighted = isSelectedJoint || isBodyASelected || isBodyBSelected;
            bool isVisible = displaySettings.showAllJoints || highlighted;

            createOrUpdateJointLines(entity, joint, isVisible, highlighted);
        }

        if (signature.test(scene->getComponentId<CameraComponent>()) && signature.test(scene->getComponentId<Transform>())){
            if (entity != camera->getEntity()){
                Transform& transform = scene->getComponent<Transform>(entity);
                CameraComponent& cameraComp = scene->getComponent<CameraComponent>(entity);

                if (transform.visible && !displaySettings.hideCameraView){
                    icons.add(GizmoIcon::Camera, transform.worldPosition, billboardScreenScale(transform.worldPosition, iconScale));
                    Quaternion rotation;
                    rotation.fromRotationMatrix(cameraComp.viewMatrix.inverse());
                    // a selected camera shows its whole frustum, near to far (Unity)
                    drawCameraFrustumLines(cameraLines, cameraComp, mainCamera == entity, selectedEntities.find(entity) != selectedEntities.end(),
                                           Matrix4::composeTRS(transform.worldPosition, rotation, Vector3(1.0f, 1.0f, 1.0f)));
                }
            }
        }

        if (signature.test(scene->getComponentId<AudioSourceComponent>()) && signature.test(scene->getComponentId<Transform>())){
            Transform& transform = scene->getComponent<Transform>(entity);


            if (transform.visible && !displaySettings.hideSoundIcons){
                icons.add(GizmoIcon::Sound, transform.worldPosition, billboardScreenScale(transform.worldPosition, iconScale));
            }
        }
    }

    flushGizmos(icons);
    flushGizmos(particleShapeLines);
    for (GizmoLineBatch* batch : lineBatches()) {
        flushGizmos(*batch);
    }

    // --- Canvas outline for a selected UI Document (documents are not pickable here:
    // they are selected in the Hierarchy) ---
    documentLines->clearLines();
    documentLines->setVisible(false);
    for (Entity entity : selEntities){
        const UIDocumentComponent* document = scene->findComponent<UIDocumentComponent>(entity);
        if (!document || document->renderMode != UIDocumentRenderMode::SCREEN || !scene->isEntityActiveInHierarchy(entity))
            continue;

        const float w = (float)canvasFrameWidth;
        const float h = (float)canvasFrameHeight;
        const Vector4 color = document->enabled ? Vector4(0.89f, 0.30f, 0.15f, 1.0f) : Vector4(0.89f, 0.30f, 0.15f, 0.4f);
        documentLines->addLine(Vector3(0, 0, 0), Vector3(w, 0, 0), color);
        documentLines->addLine(Vector3(w, 0, 0), Vector3(w, h, 0), color);
        documentLines->addLine(Vector3(w, h, 0), Vector3(0, h, 0), color);
        documentLines->addLine(Vector3(0, h, 0), Vector3(0, 0, 0), color);
        documentLines->setVisible(true);
        break;
    }

    // --- Tile outlines for selected tilemap ---
    tileLines->clearLines();
    tileLines->setVisible(false);
    if (selEntities.size() == 1){
        Entity selEntity = selEntities[0];
        Signature selSig = scene->getSignature(selEntity);
        if (selSig.test(scene->getComponentId<TilemapComponent>()) && selSig.test(scene->getComponentId<Transform>())){
            TilemapComponent& tilemap = scene->getComponent<TilemapComponent>(selEntity);
            Transform& transform = scene->getComponent<Transform>(selEntity);
            const Matrix4& modelMatrix = transform.modelMatrix;

            if (transform.visible && tilemap.numTiles > 0){
                tileLines->setVisible(true);

                const Vector4 normalColor(0.4f, 0.7f, 0.9f, 0.5f);
                const Vector4 selectedColor(1.0f, 0.8f, 0.2f, 1.0f);

                for (unsigned int i = 0; i < tilemap.numTiles; i++){
                    const TileData& tile = tilemap.tiles[i];
                    bool isSelected = ((int)i == getSelectedTileIndex() && selEntity == getSelectedTileEntity());
                    const Vector4& color = isSelected ? selectedColor : normalColor;

                    Vector3 p1 = modelMatrix * Vector3(tile.position.x, tile.position.y, 0);
                    Vector3 p2 = modelMatrix * Vector3(tile.position.x + tile.width, tile.position.y, 0);
                    Vector3 p3 = modelMatrix * Vector3(tile.position.x + tile.width, tile.position.y + tile.height, 0);
                    Vector3 p4 = modelMatrix * Vector3(tile.position.x, tile.position.y + tile.height, 0);

                    tileLines->addLine(p1, p2, color);
                    tileLines->addLine(p2, p3, color);
                    tileLines->addLine(p3, p4, color);
                    tileLines->addLine(p4, p1, color);
                }
            }
        }
    }

    updateNavigationLines(entities);
}

// Unity's Navigation > Visualize: an arrow from each navigable Button toward where each way
// leads (yellow), drawn while the preference is on
void editor::SceneRender2D::updateNavigationLines(const std::vector<Entity>& entities){
    if (!AppSettings::getUINavigationVisualize()){
        navigationLines->setVisible(false);
        return;
    }
    navigationLines->clearLines();
    UISystem& ui = *scene->getSystem<UISystem>();
    const Vector4 color(1.0f, 0.92f, 0.016f, 1.0f);
    auto center = [&](Entity entity) -> Vector3 {
        const Transform& transform = scene->getComponent<Transform>(entity);
        const RectTransformComponent& layout = scene->getComponent<RectTransformComponent>(entity);
        return transform.modelMatrix * Vector3(layout.width * 0.5f, layout.height * 0.5f, 0.0f);
    };
    bool any = false;
    for (Entity entity : entities){
        const Signature signature = scene->getSignature(entity);
        if (!signature.test(scene->getComponentId<ButtonComponent>()) || !signature.test(scene->getComponentId<RectTransformComponent>()) ||
            !signature.test(scene->getComponentId<Transform>()) || !ui.isNavigable(entity))
            continue;
        const Vector3 from = center(entity);
        for (MoveDirection dir : {MoveDirection::UP, MoveDirection::DOWN, MoveDirection::LEFT, MoveDirection::RIGHT}){
            const Entity target = ui.findSelectableOnDirection(entity, dir);
            if (target == NULL_ENTITY || !scene->findComponent<RectTransformComponent>(target))
                continue;
            // from a third of the way to two thirds: the two arrows between a pair stay apart
            const Vector3 to = center(target);
            const Vector3 start = from + (to - from) * 0.2f;
            const Vector3 end = from + (to - from) * 0.8f;
            navigationLines->addLine(start, end, color);
            Vector3 back = start - end;
            const float length = back.length();
            if (length > 0.0f){
                back = back * (std::min(12.0f, length * 0.3f) / length);
                const Vector3 side(-back.y * 0.5f, back.x * 0.5f, 0.0f);
                navigationLines->addLine(end, end + back + side, color);
                navigationLines->addLine(end, end + back - side, color);
            }
            any = true;
        }
    }
    if (!any)
        navigationLines->addLine(Vector3::ZERO, Vector3::ZERO, Vector4::ZERO);
    navigationLines->setVisible(any);
}

void editor::SceneRender2D::zoomAtPosition(float width, float height, Vector2 pos, float zoomFactor){
    float shiftX = pos.x * zoom * (1.0f - zoomFactor);
    float shiftY = pos.y * zoom * (1.0f - zoomFactor);

    camera->slide(shiftX);
    camera->slideUp(shiftY);

    zoom *= zoomFactor;

    applyZoomProjection();
}

float editor::SceneRender2D::getZoom() const {
    return zoom;
}

void editor::SceneRender2D::setZoom(float zoom) {
    this->zoom = zoom;

    if (viewportWidth > 0 && viewportHeight > 0) {
        applyZoomProjection();
    }
}
