// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The runtime collider, joint and contact lines of PhysicsSystem (Physics.debugDraw,
// physics-3d 6.1.12): a LineRenderer on a system entity of the scene (not in the Hierarchy,
// never saved), filled once a frame from the live bodies while the flag is on.

#include "PhysicsSystem.h"
#include "component/Joint3DComponent.h"
#include "Scene.h"
#include "component/LineRendererComponent.h"
#include "component/Transform.h"
#include "object/LineRenderer.h"
#include "physics/PhysicsDebugLines.h"
#include "registry/PhysicsLayers.h"
#include "subsystem/VehicleSystem.h"

#include <algorithm>
#include <chrono>

#ifdef JASPERENGINE_PHYSICS_3D
#include "util/JoltPhysicsAux.h"
#endif

using namespace jasperengine;

namespace {

    const Vector4 colorDynamic(0.25f, 0.95f, 0.3f, 1.0f);
    const Vector4 colorSleeping(0.1f, 0.45f, 0.15f, 1.0f);
    const Vector4 colorKinematic(0.35f, 0.55f, 1.0f, 1.0f);
    const Vector4 colorStatic(0.6f, 0.6f, 0.6f, 1.0f);
    const Vector4 colorTrigger(1.0f, 0.9f, 0.15f, 1.0f);
    const Vector4 colorJoint(1.0f, 0.25f, 1.0f, 1.0f);
    const Vector4 colorJointLimit(1.0f, 0.55f, 1.0f, 1.0f);
    const Vector4 colorContact(1.0f, 0.2f, 0.2f, 1.0f);

#ifdef JASPERENGINE_PHYSICS_2D
    struct Draw2DContext{
        LineRenderer* lines;
        float pointsToMeter;
    };

    Vector4 hexColor(b2HexColor color){
        const uint32_t c = (uint32_t)color;
        return Vector4(((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f, 1.0f);
    }

    Vector3 point2D(const Draw2DContext* context, b2Vec2 p){
        return Vector3(p.x * context->pointsToMeter, p.y * context->pointsToMeter, 0.0f);
    }

    void drawPolygon2D(const b2Vec2* vertices, int count, b2HexColor color, void* data){
        Draw2DContext* context = static_cast<Draw2DContext*>(data);
        for (int i = 0; i < count; i++){
            context->lines->addLine(point2D(context, vertices[i]), point2D(context, vertices[(i + 1) % count]), hexColor(color));
        }
    }

    void drawSolidPolygon2D(b2Transform transform, const b2Vec2* vertices, int count, float radius, b2HexColor color, void* data){
        (void)radius;
        std::vector<b2Vec2> world((size_t)count);
        for (int i = 0; i < count; i++){
            world[(size_t)i] = b2TransformPoint(transform, vertices[i]);
        }
        drawPolygon2D(world.data(), count, color, data);
    }

    void drawCircle2D(b2Vec2 center, float radius, b2HexColor color, void* data){
        Draw2DContext* context = static_cast<Draw2DContext*>(data);
        const int segments = 20;
        for (int i = 0; i < segments; i++){
            const float a0 = 2.0f * (float)M_PI * i / segments;
            const float a1 = 2.0f * (float)M_PI * (i + 1) / segments;
            context->lines->addLine(point2D(context, b2Vec2{center.x + radius * std::cos(a0), center.y + radius * std::sin(a0)}),
                                    point2D(context, b2Vec2{center.x + radius * std::cos(a1), center.y + radius * std::sin(a1)}), hexColor(color));
        }
    }

    void drawSolidCircle2D(b2Transform transform, float radius, b2HexColor color, void* data){
        drawCircle2D(transform.p, radius, color, data);
        Draw2DContext* context = static_cast<Draw2DContext*>(data);
        // a radius shows the turn
        const b2Vec2 edge = b2TransformPoint(transform, b2Vec2{radius, 0.0f});
        context->lines->addLine(point2D(context, transform.p), point2D(context, edge), hexColor(color));
    }

    void drawSolidCapsule2D(b2Vec2 p1, b2Vec2 p2, float radius, b2HexColor color, void* data){
        Draw2DContext* context = static_cast<Draw2DContext*>(data);
        drawCircle2D(p1, radius, color, data);
        drawCircle2D(p2, radius, color, data);
        b2Vec2 axis = b2Sub(p2, p1);
        const float length = b2Length(axis);
        if (length > 0.0f){
            axis = b2MulSV(1.0f / length, axis);
            const b2Vec2 side = b2Vec2{-axis.y * radius, axis.x * radius};
            context->lines->addLine(point2D(context, b2Add(p1, side)), point2D(context, b2Add(p2, side)), hexColor(color));
            context->lines->addLine(point2D(context, b2Sub(p1, side)), point2D(context, b2Sub(p2, side)), hexColor(color));
        }
    }

    void drawSegment2D(b2Vec2 p1, b2Vec2 p2, b2HexColor color, void* data){
        Draw2DContext* context = static_cast<Draw2DContext*>(data);
        context->lines->addLine(point2D(context, p1), point2D(context, p2), hexColor(color));
    }

    void drawTransform2D(b2Transform transform, void* data){
        (void)transform;
        (void)data;
    }

    void drawPoint2D(b2Vec2 p, float size, b2HexColor color, void* data){
        Draw2DContext* context = static_cast<Draw2DContext*>(data);
        // size is in pixels for Box2D's own renderer: a small cross (metres) here
        (void)size;
        const float half = 0.05f;
        context->lines->addLine(point2D(context, b2Vec2{p.x - half, p.y}), point2D(context, b2Vec2{p.x + half, p.y}), hexColor(color));
        context->lines->addLine(point2D(context, b2Vec2{p.x, p.y - half}), point2D(context, b2Vec2{p.x, p.y + half}), hexColor(color));
    }

    void drawString2D(b2Vec2 p, const char* s, void* data){
        (void)p;
        (void)s;
        (void)data;
    }
#endif

}

size_t PhysicsSystem::getDebugLineCount() const{
    if (debugLinesEntity == NULL_ENTITY || !scene->isEntityCreated(debugLinesEntity)){
        return 0;
    }
    const LineRendererComponent* lines = scene->findComponent<LineRendererComponent>(debugLinesEntity);
    return (lines && debugLinesShown) ? lines->lines.size() : 0;
}

#ifdef JASPERENGINE_PHYSICS_3D
void PhysicsSystem::recordDebugContact3D(const JPH::ContactManifold& manifold){
    if (!Physics::isDebugDrawContacts()){
        return;
    }
    std::lock_guard<std::mutex> lock(debugContactsMutex3D);
    const JPH::Vec3 normal = manifold.mWorldSpaceNormal;
    for (JPH::uint i = 0; i < manifold.mRelativeContactPointsOn1.size(); i++){
        const JPH::RVec3 point = manifold.GetWorldSpaceContactPointOn1(i);
        debugContacts3D.emplace_back(Vector3(point.GetX(), point.GetY(), point.GetZ()), Vector3(normal.GetX(), normal.GetY(), normal.GetZ()));
    }
}
#endif

namespace {
    double debugLinesClock(){
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
}

void PhysicsSystem::addTimedDebugLines(const std::vector<std::pair<Vector3, Vector3>>& lines, const Vector4& color, float seconds){
    const double until = debugLinesClock() + (double)seconds;
    for (const auto& line : lines){
        timedDebugLines.push_back({line.first, line.second, color, until});
    }
}

bool PhysicsSystem::pruneTimedDebugLines(){
    if (timedDebugLines.empty()){
        return false;
    }
    const double now = debugLinesClock();
    timedDebugLines.erase(std::remove_if(timedDebugLines.begin(), timedDebugLines.end(),
                                         [now](const TimedDebugLine& line){ return line.until <= now; }),
                          timedDebugLines.end());
    return !timedDebugLines.empty();
}

void PhysicsSystem::updateDebugLines(){
    const bool on = Physics::isDebugDraw();
    const bool timed = pruneTimedDebugLines();
    if (!on && !timed){
        if (debugLinesShown && debugLinesEntity != NULL_ENTITY && scene->isEntityCreated(debugLinesEntity)){
            LineRenderer lines(scene, debugLinesEntity);
            lines.clearLines();
            lines.setVisible(false);
        }
        debugLinesShown = false;
#ifdef JASPERENGINE_PHYSICS_3D
        debugEdgeCaches3D.clear();
#endif
        return;
    }

    if (debugLinesEntity == NULL_ENTITY || !scene->isEntityCreated(debugLinesEntity) || !scene->findComponent<LineRendererComponent>(debugLinesEntity)){
        debugLinesEntity = scene->createSystemEntity();
        if (debugLinesEntity == NULL_ENTITY){
            return;
        }
        scene->addComponent<Transform>(debugLinesEntity, Transform());
        scene->addComponent<LineRendererComponent>(debugLinesEntity, LineRendererComponent());
        scene->setEntityName(debugLinesEntity, "Physics Debug Lines");
    }
    LineRenderer lines(scene, debugLinesEntity);
    lines.clearLines();
    lines.setVisible(true);
    debugLinesShown = true;

    for (const TimedDebugLine& line : timedDebugLines){
        lines.addLine(line.from, line.to, line.color);
    }

#ifdef JASPERENGINE_PHYSICS_3D
    if (on && world3DReady){
        JPH::BodyInterface& bodyInterface = getBodyInterface3D();
        auto bodies3d = scene->getComponentArray<RigidbodyComponent>();
        for (size_t i = 0; i < bodies3d->size(); i++){
            RigidbodyComponent& body = bodies3d->getComponentFromIndex(i);
            const Entity entity = bodies3d->getEntity(i);
            Transform* transform = scene->findComponent<Transform>(entity);
            if (!transform){
                continue;
            }
            // S96 WP-C: an attached child's colliders where its GameObject is drawn, in the
            // colour of the body they joined
            const RigidbodyComponent* owner = &body;
            if (body.body.IsInvalid() && body.attachedTo != NULL_ENTITY && scene->isEntityCreated(body.attachedTo)){
                owner = scene->findComponent<RigidbodyComponent>(body.attachedTo);
            }
            if (!owner || owner->body.IsInvalid() || !bodyInterface.IsAdded(owner->body)){
                continue;
            }
            PhysicsDebugLines::Pose pose;
            if (owner == &body){
                JPH::RVec3 position;
                JPH::Quat rotation;
                bodyInterface.GetPositionAndRotation(body.body, position, rotation);
                pose.position = Vector3(position.GetX(), position.GetY(), position.GetZ());
                pose.rotation = Quaternion(rotation.GetW(), rotation.GetX(), rotation.GetY(), rotation.GetZ());
            }else{
                pose.position = transform->worldPosition;
                pose.rotation = transform->worldRotation;
            }
            pose.scale = transform->worldScale;

            Vector4 color = colorStatic;
            if (owner->type == BodyType::KINEMATIC){
                color = colorKinematic;
            }else if (owner->type == BodyType::DYNAMIC){
                color = bodyInterface.IsActive(owner->body) ? colorDynamic : colorSleeping;
            }
            const Vector4 bodyColor = owner->sensor ? colorTrigger : color;
            PhysicsDebugLines::bodyEdges(scene, entity, *transform, body, pose, debugEdgeCaches3D[entity],
                [&](const Vector3& a, const Vector3& b, size_t collider){
                    lines.addLine(a, b, (collider < body.colliders.size() && body.colliders[collider].isTrigger) ? colorTrigger : bodyColor);
                });
        }

        auto poseOf = [&](Entity bodyEntity, PhysicsDebugLines::Pose& pose) -> bool {
            RigidbodyComponent* body = scene->findComponent<RigidbodyComponent>(bodyEntity);
            Transform* transform = scene->findComponent<Transform>(bodyEntity);
            if (!body || body->body.IsInvalid() || !transform){
                return false;
            }
            JPH::RVec3 position;
            JPH::Quat rotation;
            bodyInterface.GetPositionAndRotation(body->body, position, rotation);
            pose.position = Vector3(position.GetX(), position.GetY(), position.GetZ());
            pose.rotation = Quaternion(rotation.GetW(), rotation.GetX(), rotation.GetY(), rotation.GetZ());
            pose.scale = transform->worldScale;
            return true;
        };
        auto joints3d = scene->getComponentArray<Joint3DComponent>();
        for (size_t i = 0; i < joints3d->size(); i++){
            const Joint3DComponent& joint = joints3d->getComponentFromIndex(i);
            if (!joint.joint){
                continue;
            }
            PhysicsDebugLines::jointEdges(scene, joint, poseOf, [&](const Vector3& a, const Vector3& b, int kind){
                lines.addLine(a, b, kind == 3 ? colorJointLimit : colorJoint);
            });
        }

        // S96 WP-A: the WheelColliders' circles and travels
        vehicles3D->debugLines([&](const Vector3& a, const Vector3& b){ lines.addLine(a, b, colorJoint); });

        if (Physics::isDebugDrawContacts()){
            std::lock_guard<std::mutex> lock(debugContactsMutex3D);
            for (const auto& contact : debugContacts3D){
                PhysicsDebugLines::crossMark(contact.first, 0.04f, [&](const Vector3& a, const Vector3& b){ lines.addLine(a, b, colorContact); });
                lines.addLine(contact.first, contact.first + contact.second * 0.25f, colorContact);
            }
        }

        // caches of bodies gone
        for (auto it = debugEdgeCaches3D.begin(); it != debugEdgeCaches3D.end();){
            if (!scene->isEntityCreated(it->first) || !scene->findComponent<RigidbodyComponent>(it->first)){
                it = debugEdgeCaches3D.erase(it);
            }else{
                ++it;
            }
        }
    }
#endif

#ifdef JASPERENGINE_PHYSICS_2D
    if (on && b2World_IsValid(world2D)){
        Draw2DContext context{&lines, pointsToMeterScale2D};
        b2DebugDraw draw = b2DefaultDebugDraw();
        draw.DrawPolygon = drawPolygon2D;
        draw.DrawSolidPolygon = drawSolidPolygon2D;
        draw.DrawCircle = drawCircle2D;
        draw.DrawSolidCircle = drawSolidCircle2D;
        draw.DrawSolidCapsule = drawSolidCapsule2D;
        draw.DrawSegment = drawSegment2D;
        draw.DrawTransform = drawTransform2D;
        draw.DrawPoint = drawPoint2D;
        draw.DrawString = drawString2D;
        draw.context = &context;
        draw.drawShapes = true;
        draw.drawJoints = true;
        draw.drawJointExtras = false;
        draw.drawAABBs = false;
        draw.drawMass = false;
        draw.drawContacts = Physics::isDebugDrawContacts();
        draw.drawContactNormals = Physics::isDebugDrawContacts();
        b2World_Draw(world2D, &draw);
    }
#endif
}
