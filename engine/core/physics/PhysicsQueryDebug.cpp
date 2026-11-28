// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "PhysicsQueryDebug.h"
#include "physics/PhysicsDebugLines.h"
#include "subsystem/PhysicsSystem.h"
#include "jobs/JobSystem.h"
#include "Engine.h"
#include "component/Transform.h"
#include "Scene.h"
#include "Log.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace {

    const Vector4 queryHitColor(0.2f, 1.0f, 0.35f, 1.0f);
    const Vector4 queryMissColor(1.0f, 0.25f, 0.2f, 1.0f);
    constexpr int QUERY_CIRCLE_SEGMENTS = 24;

    using QueryEmit = std::function<void(const Vector3&, const Vector3&)>;

    void queryCircle(const Vector3& center, const Vector3& axisU, const Vector3& axisV, float radius, const QueryEmit& emit){
        Vector3 previous = center + axisU * radius;
        for (int i = 1; i <= QUERY_CIRCLE_SEGMENTS; i++){
            const float angle = 2.0f * (float)M_PI * (float)i / (float)QUERY_CIRCLE_SEGMENTS;
            const Vector3 next = center + axisU * (radius * std::cos(angle)) + axisV * (radius * std::sin(angle));
            emit(previous, next);
            previous = next;
        }
    }

    void querySphere(const Vector3& center, float radius, const QueryEmit& emit){
        if (!(radius > 0.0f)){
            return;
        }
        queryCircle(center, Vector3(1, 0, 0), Vector3(0, 1, 0), radius, emit);
        queryCircle(center, Vector3(1, 0, 0), Vector3(0, 0, 1), radius, emit);
        queryCircle(center, Vector3(0, 1, 0), Vector3(0, 0, 1), radius, emit);
    }

    void queryBox(const Vector3& center, const Vector3& half, const Quaternion& rotation, const QueryEmit& emit){
        Vector3 corners[8];
        for (int i = 0; i < 8; i++){
            const Vector3 local((i & 1) ? half.x : -half.x, (i & 2) ? half.y : -half.y, (i & 4) ? half.z : -half.z);
            corners[i] = center + rotation * local;
        }
        for (int i = 0; i < 8; i++){
            for (int bit = 1; bit < 8; bit <<= 1){
                if (!(i & bit)){
                    emit(corners[i], corners[i | bit]);
                }
            }
        }
    }

    void queryCapsule(const Vector3& a, const Vector3& b, float radius, const QueryEmit& emit){
        querySphere(a, radius, emit);
        querySphere(b, radius, emit);
        Vector3 axis = b - a;
        const float length = axis.length();
        if (!(length > 0.0f)){
            return;
        }
        axis = axis / length;
        Vector3 side = axis.crossProduct(std::fabs(axis.y) < 0.9f ? Vector3(0, 1, 0) : Vector3(1, 0, 0)).normalized();
        const Vector3 other = axis.crossProduct(side);
        const Vector3 offsets[4] = {side * radius, side * -radius, other * radius, other * -radius};
        for (const Vector3& offset : offsets){
            emit(a + offset, b + offset);
        }
    }

    void queryRect2D(const Vector3& center, const Vector3& half, float angleDegrees, const QueryEmit& emit){
        const float angle = angleDegrees * (float)M_PI / 180.0f;
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        Vector3 corners[4];
        const float signs[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (int i = 0; i < 4; i++){
            const float x = signs[i][0] * half.x;
            const float y = signs[i][1] * half.y;
            corners[i] = Vector3(center.x + x * c - y * s, center.y + x * s + y * c, 0.0f);
        }
        for (int i = 0; i < 4; i++){
            emit(corners[i], corners[(i + 1) % 4]);
        }
    }

    void queryCapsule2D(const Vector3& a, const Vector3& b, float radius, const QueryEmit& emit){
        queryCircle(a, Vector3(1, 0, 0), Vector3(0, 1, 0), radius, emit);
        queryCircle(b, Vector3(1, 0, 0), Vector3(0, 1, 0), radius, emit);
        Vector3 axis = b - a;
        const float length = axis.length();
        if (!(length > 0.0f)){
            return;
        }
        const Vector3 side(-axis.y / length * radius, axis.x / length * radius, 0.0f);
        emit(a + side, b + side);
        emit(a - side, b - side);
    }

    // the shape moved by offset (a cast's end)
    void queryShapeLines(const physics::QueryShape& shape, const Vector3& offset, const QueryEmit& emit){
        using Type = physics::QueryShape::Type;
        const Vector3 a = shape.a + offset;
        switch (shape.type){
            case Type::Sphere: querySphere(a, shape.radius, emit); break;
            case Type::Box: queryBox(a, shape.halfExtents, shape.rotation, emit); break;
            case Type::Capsule: queryCapsule(a, shape.b + offset, shape.radius, emit); break;
            case Type::Point:
            case Type::Point2D: PhysicsDebugLines::crossMark(a, 0.1f, emit); break;
            case Type::Circle2D:
                if (shape.radius > 0.0f) queryCircle(a, Vector3(1, 0, 0), Vector3(0, 1, 0), shape.radius, emit);
                break;
            case Type::Box2D: queryRect2D(a, shape.halfExtents, shape.angle, emit); break;
            case Type::Capsule2D: {
                Vector2 p1, p2;
                float radius;
                physics::capsule2DSegment(Vector2(a.x, a.y), Vector2(shape.halfExtents.x * 2.0f, shape.halfExtents.y * 2.0f),
                                          shape.direction2D, shape.angle, p1, p2, radius);
                queryCapsule2D(Vector3(p1.x, p1.y, 0.0f), Vector3(p2.x, p2.y, 0.0f), radius, emit);
                break;
            }
            case Type::Area2D: {
                const Vector3 b = shape.b + offset;
                const Vector3 centre((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, 0.0f);
                queryRect2D(centre, Vector3(std::fabs(b.x - a.x) * 0.5f, std::fabs(b.y - a.y) * 0.5f, 0.0f), 0.0f, emit);
                break;
            }
        }
    }

    // a size for the hit crosses, from the shape's own
    float queryMarkSize(const physics::QueryShape& shape){
        const float extent = std::max({shape.radius, shape.halfExtents.x, shape.halfExtents.y, shape.halfExtents.z});
        return std::max(0.05f, extent * 0.15f);
    }

}

void PhysicsQueryDebug::record(const Entry& entry, double now, float lifetime){
    if (!entries){
        entries.reset(new Entry[CAPACITY]);
        next = 0;
        count = 0;
    }
    Entry& slot = entries[next];
    slot = entry;
    slot.expires = now + (double)lifetime;
    next = (next + 1) % CAPACITY;
    count = std::min(count + 1, CAPACITY);
    offSince = -1.0;
}

void PhysicsQueryDebug::forEachLive(double now, const std::function<void(const Entry&)>& visit) const{
    if (!entries){
        return;
    }
    const size_t first = (next + CAPACITY - count) % CAPACITY;
    for (size_t i = 0; i < count; i++){
        const Entry& entry = entries[(first + i) % CAPACITY];
        if (entry.expires > now){
            visit(entry);
        }
    }
}

size_t PhysicsQueryDebug::liveCount(double now) const{
    size_t live = 0;
    forEachLive(now, [&](const Entry&){ live++; });
    return live;
}

void PhysicsQueryDebug::releaseIfIdle(double now, bool debugDrawOn){
    if (debugDrawOn || !entries){
        offSince = -1.0;
        return;
    }
    if (offSince < 0.0){
        offSince = now;
        return;
    }
    if (now - offSince >= RELEASE_AFTER && liveCount(now) == 0){
        entries.reset();
        next = 0;
        count = 0;
        offSince = -1.0;
    }
}

void PhysicsQueryDebug::emitLines(const Entry& entry, const std::function<void(const Vector3&, const Vector3&, const Vector4&)>& emit){
    const Vector4& color = entry.hit ? queryHitColor : queryMissColor;
    const QueryEmit line = [&](const Vector3& from, const Vector3& to){ emit(from, to, color); };
    queryShapeLines(entry.shape, Vector3::ZERO, line);
    if (entry.cast){
        const Vector3 sweep = entry.direction * entry.distance;
        // a ray is a cast of a zero-radius sphere: its sweep is the whole drawing
        queryShapeLines(entry.shape, sweep, line);
        const Vector3 from = entry.shape.type == physics::QueryShape::Type::Area2D ? (entry.shape.a + entry.shape.b) * 0.5f : entry.shape.a;
        line(from, from + sweep);
    }
    const float mark = queryMarkSize(entry.shape);
    for (uint8_t i = 0; i < entry.hitPoints; i++){
        PhysicsDebugLines::crossMark(entry.points[i], mark, line);
    }
}

bool physics::recordingQueries(){
    return Physics::isDebugDrawQueries() && Log::isMainThread() && !jobs::isExecutingJob();
}

void physics::recordQuery(Scene* scene, const QueryShape& shape, const Vector3& direction, float distance, bool cast,
                          const Vector3* hitPoints, size_t hitCount, bool hit){
    if (!scene || !recordingQueries()){
        return;
    }
    PhysicsQueryDebug::Entry entry;
    entry.shape = shape;
    entry.direction = direction;
    entry.distance = distance;
    entry.cast = cast;
    entry.hit = hit;
    entry.hitPoints = (uint8_t)std::min(hitCount, PhysicsQueryDebug::MAX_HIT_POINTS);
    for (uint8_t i = 0; i < entry.hitPoints; i++){
        entry.points[i] = hitPoints[i];
    }
    scene->getSystem<PhysicsSystem>()->getQueryDebug().record(entry, Engine::getUnscaledTime(), PhysicsQueryDebug::LIFETIME);
}

void physics::recordOverlap(Scene* scene, const QueryShape& shape, bool hit, const Entity* entities, size_t entityCount){
    if (!scene){
        return;
    }
    Vector3 points[PhysicsQueryDebug::MAX_HIT_POINTS];
    size_t pointCount = 0;
    for (size_t i = 0; i < entityCount && pointCount < PhysicsQueryDebug::MAX_HIT_POINTS; i++){
        if (const Transform* transform = scene->findComponent<Transform>(entities[i])){
            points[pointCount++] = transform->worldPosition;
        }
    }
    recordQuery(scene, shape, Vector3::ZERO, 0.0f, false, points, pointCount, hit);
}

void physics::noteCast(Scene* scene, const QueryShape& shape, const Vector3& direction, float distance, const RayReturn* hits, size_t hitCount){
    Physics::countQuery(hitCount);
    if (!recordingQueries()){
        return;
    }
    Vector3 points[PhysicsQueryDebug::MAX_HIT_POINTS];
    size_t pointCount = 0;
    for (size_t i = 0; i < hitCount && pointCount < PhysicsQueryDebug::MAX_HIT_POINTS; i++){
        points[pointCount++] = hits[i].point;
    }
    // the sweep ends at the farthest hit drawn, or goes the whole way
    const float reach = hitCount > 0 ? hits[hitCount - 1].distance : distance;
    const float length = direction.length();
    recordQuery(scene, shape, length > 0.0f ? direction / length : direction, reach, true, points, pointCount, hitCount > 0);
}
