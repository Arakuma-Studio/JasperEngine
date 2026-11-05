// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PhysicsDebugLines.h"
#include "model/ModelCpuData.h"

#include "Scene.h"
#include "buffer/Buffer.h"
#include "component/Joint3DComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "util/Angle.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <string>

using namespace jasperengine;

uint64_t PhysicsDebugLines::quantizeHullKey(const Vector3& p) {
    auto q = [](float v) { return int32_t(std::lround(v * PhysicsDebugLines::kHullQuantizeScale)); };
    uint64_t x = uint32_t(q(p.x));
    uint64_t y = uint32_t(q(p.y));
    uint64_t z = uint32_t(q(p.z));
    return (x * 73856093u) ^ (y * 19349663u) ^ (z * 83492791u);
}

void PhysicsDebugLines::pushUniqueHullPoint(std::vector<Vector3>& points, std::unordered_set<uint64_t>& seen, const Vector3& p) {
    if (!p.isValid()) return;
    if (seen.insert(quantizeHullKey(p)).second) {
        points.push_back(p);
    }
}

// Reduce a large point cloud to its extremes along a fixed direction set.
// The convex hull of these extremes is contained in the original hull and
// is sufficient for visualization while bounding cost.
void PhysicsDebugLines::reduceToExtremes(std::vector<Vector3>& points) {
    if (points.size() <= PhysicsDebugLines::kMaxHullInputPoints) return;

    // A 5^3 lattice of directions, dense enough that the drawn hull stays close to
    // the one Jolt builds from the same vertices.
    static const std::vector<Vector3> directions = [](){
        std::vector<Vector3> dirs;
        for (int x = -2; x <= 2; x++){
            for (int y = -2; y <= 2; y++){
                for (int z = -2; z <= 2; z++){
                    if (x != 0 || y != 0 || z != 0) dirs.push_back(Vector3(x, y, z));
                }
            }
        }
        return dirs;
    }();
    const int dirCount = int(directions.size());

    std::vector<int> bestIndex(dirCount, 0);
    std::vector<float> bestDot(dirCount, -std::numeric_limits<float>::infinity());

    for (size_t i = 0; i < points.size(); i++) {
        for (int d = 0; d < dirCount; d++) {
            float dot = points[i].dotProduct(directions[d]);
            if (dot > bestDot[d]) {
                bestDot[d] = dot;
                bestIndex[d] = int(i);
            }
        }
    }

    std::vector<Vector3> reduced;
    std::unordered_set<uint64_t> seen;
    reduced.reserve(dirCount);
    for (int d = 0; d < dirCount; d++) {
        pushUniqueHullPoint(reduced, seen, points[bestIndex[d]]);
    }
    points = std::move(reduced);
}

// Builds convex hull edges from a point set and emits each unique edge once.
// Uses an incremental QuickHull with axis-extreme seeding, cached face
// normals and a horizon-edge map. Falls back to a 2D Andrew monotone chain
// when the input is coplanar.
void PhysicsDebugLines::buildConvexHullEdges(std::vector<Vector3> points, const Emit& emit) {
    reduceToExtremes(points);
    if (points.size() < 2) return;
    if (points.size() == 2) { emit(points[0], points[1]); return; }

    Vector3 minPt = points[0];
    Vector3 maxPt = points[0];
    for (const Vector3& p : points) {
        minPt.x = std::min(minPt.x, p.x); minPt.y = std::min(minPt.y, p.y); minPt.z = std::min(minPt.z, p.z);
        maxPt.x = std::max(maxPt.x, p.x); maxPt.y = std::max(maxPt.y, p.y); maxPt.z = std::max(maxPt.z, p.z);
    }
    Vector3 extent = maxPt - minPt;
    float maxExtent = std::max(extent.x, std::max(extent.y, extent.z));
    float eps = std::max(1e-5f, maxExtent * 1e-5f);
    float epsSq = eps * eps;

    // Seed pair from the 6 axis extremes (min/max over x, y, z).
    int axisExt[6] = {0, 0, 0, 0, 0, 0};
    {
        float bestVal[6] = {points[0].x, -points[0].x, points[0].y, -points[0].y, points[0].z, -points[0].z};
        for (size_t i = 1; i < points.size(); i++) {
            float v[6] = {points[i].x, -points[i].x, points[i].y, -points[i].y, points[i].z, -points[i].z};
            for (int k = 0; k < 6; k++) {
                if (v[k] > bestVal[k]) { bestVal[k] = v[k]; axisExt[k] = int(i); }
            }
        }
    }

    int i0 = 0, i1 = 0;
    float maxDistSq = 0.0f;
    for (int a = 0; a < 6; a++) {
        for (int b = a + 1; b < 6; b++) {
            float d = points[axisExt[a]].squaredDistance(points[axisExt[b]]);
            if (d > maxDistSq) { maxDistSq = d; i0 = axisExt[a]; i1 = axisExt[b]; }
        }
    }
    if (maxDistSq <= epsSq) return;

    Vector3 baseLine = points[i1] - points[i0];
    int i2 = -1;
    float maxLineSq = 0.0f;
    for (size_t i = 0; i < points.size(); i++) {
        float d = (points[i] - points[i0]).crossProduct(baseLine).squaredLength() / maxDistSq;
        if (d > maxLineSq) { maxLineSq = d; i2 = int(i); }
    }
    if (i2 < 0 || maxLineSq <= epsSq) { emit(points[i0], points[i1]); return; }

    Vector3 planeNormal = (points[i1] - points[i0]).crossProduct(points[i2] - points[i0]);
    float planeNormalLen = planeNormal.length();

    int i3 = -1;
    float maxPlaneDist = 0.0f;
    for (size_t i = 0; i < points.size(); i++) {
        float d = std::fabs(planeNormal.dotProduct(points[i] - points[i0]) / planeNormalLen);
        if (d > maxPlaneDist) { maxPlaneDist = d; i3 = int(i); }
    }

    // Coplanar fallback: 2D convex hull (Andrew's monotone chain).
    if (i3 < 0 || maxPlaneDist <= eps) {
        Vector3 absN(std::fabs(planeNormal.x), std::fabs(planeNormal.y), std::fabs(planeNormal.z));
        int drop = (absN.y > absN.x && absN.y >= absN.z) ? 1 : ((absN.z > absN.x) ? 2 : 0);
        struct P2 { float x, y; int idx; };
        std::vector<P2> pp;
        pp.reserve(points.size());
        for (size_t i = 0; i < points.size(); i++) {
            const Vector3& p = points[i];
            float px = drop == 0 ? p.y : p.x;
            float py = drop == 2 ? p.y : p.z;
            pp.push_back({px, py, int(i)});
        }
        std::sort(pp.begin(), pp.end(), [](const P2& a, const P2& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
        auto cross2 = [](const P2& a, const P2& b, const P2& c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); };
        std::vector<P2> poly;
        for (const P2& p : pp) {
            while (poly.size() >= 2 && cross2(poly[poly.size() - 2], poly.back(), p) <= 0.0f) poly.pop_back();
            poly.push_back(p);
        }
        size_t lower = poly.size();
        for (auto it = pp.rbegin(); it != pp.rend(); ++it) {
            while (poly.size() > lower && cross2(poly[poly.size() - 2], poly.back(), *it) <= 0.0f) poly.pop_back();
            poly.push_back(*it);
        }
        if (!poly.empty()) poly.pop_back();
        for (size_t i = 0; i < poly.size(); i++) {
            emit(points[poly[i].idx], points[poly[(i + 1) % poly.size()].idx]);
        }
        return;
    }

    Vector3 interior = (points[i0] + points[i1] + points[i2] + points[i3]) * 0.25f;

    struct Face { int a, b, c; Vector3 n; bool removed; };
    std::vector<Face> faces;
    faces.reserve(64);

    auto pushFace = [&](int a, int b, int c) {
        Vector3 n = (points[b] - points[a]).crossProduct(points[c] - points[a]);
        if (n.squaredLength() <= epsSq) return;
        if (n.dotProduct(interior - points[a]) > 0.0f) { std::swap(b, c); n = -n; }
        faces.push_back({a, b, c, n, false});
    };

    pushFace(i0, i1, i2);
    pushFace(i0, i3, i1);
    pushFace(i0, i2, i3);
    pushFace(i1, i3, i2);

    auto edgeKey = [](int a, int b) { return (uint64_t(uint32_t(a)) << 32) | uint32_t(b); };

    std::vector<int> visible;
    std::unordered_set<uint64_t> horizon;

    for (size_t pi = 0; pi < points.size(); pi++) {
        if (int(pi) == i0 || int(pi) == i1 || int(pi) == i2 || int(pi) == i3) continue;

        visible.clear();
        for (size_t fi = 0; fi < faces.size(); fi++) {
            const Face& f = faces[fi];
            if (f.removed) continue;
            if (f.n.dotProduct(points[pi] - points[f.a]) > eps * f.n.length()) {
                visible.push_back(int(fi));
            }
        }
        if (visible.empty()) continue;

        horizon.clear();
        auto addEdge = [&](int s, int e) {
            uint64_t rev = edgeKey(e, s);
            auto it = horizon.find(rev);
            if (it != horizon.end()) { horizon.erase(it); return; }
            horizon.insert(edgeKey(s, e));
        };
        for (int fi : visible) {
            Face& f = faces[fi];
            addEdge(f.a, f.b); addEdge(f.b, f.c); addEdge(f.c, f.a);
            f.removed = true;
        }
        for (uint64_t key : horizon) {
            int s = int(uint32_t(key >> 32));
            int e = int(uint32_t(key));
            pushFace(s, e, int(pi));
        }
    }

    std::unordered_set<uint64_t> drawn;
    auto emitEdge = [&](int a, int b) {
        if (a == b) return;
        int lo = std::min(a, b), hi = std::max(a, b);
        if (drawn.insert(edgeKey(lo, hi)).second) emit(points[a], points[b]);
    };
    for (const Face& f : faces) {
        if (f.removed) continue;
        emitEdge(f.a, f.b); emitEdge(f.b, f.c); emitEdge(f.c, f.a);
    }
}

void PhysicsDebugLines::bodyEdges(Scene* scene, Entity entity, const Transform& transform, const RigidbodyComponent& body, const Pose& pose,
                                  std::vector<EdgeCache>& caches, const std::function<void(const Vector3&, const Vector3&, size_t)>& emit) {
    if (body.colliders.empty()) {
        return;
    }

    // must match how PhysicsSystem sizes the Jolt shape, parent scale included
    const Vector3 entityScale = pose.scale;
    const Vector3 absScale(std::fabs(entityScale.x), std::fabs(entityScale.y), std::fabs(entityScale.z));
    const Matrix4 bodyMatrix = Matrix4::translateMatrix(pose.position) * pose.rotation.getRotationMatrix();
    size_t colliderIndex = 0;

    auto maxScaleXZ = [&](){
        return std::max(absScale.x, absScale.z);
    };

    auto maxScaleXYZ = [&](){
        return std::max(absScale.x, std::max(absScale.y, absScale.z));
    };

    auto addTransformedLine = [&](const Matrix4& shapeMatrix, const Vector3& from, const Vector3& to) {
        Vector3 worldFrom = bodyMatrix * (shapeMatrix * from);
        Vector3 worldTo = bodyMatrix * (shapeMatrix * to);
        emit(worldFrom, worldTo, colliderIndex);
    };

    auto addBox = [&](const Matrix4& shapeMatrix, const Vector3& minPt, const Vector3& maxPt) {
        Vector3 p000(minPt.x, minPt.y, minPt.z);
        Vector3 p100(maxPt.x, minPt.y, minPt.z);
        Vector3 p110(maxPt.x, maxPt.y, minPt.z);
        Vector3 p010(minPt.x, maxPt.y, minPt.z);

        Vector3 p001(minPt.x, minPt.y, maxPt.z);
        Vector3 p101(maxPt.x, minPt.y, maxPt.z);
        Vector3 p111(maxPt.x, maxPt.y, maxPt.z);
        Vector3 p011(minPt.x, maxPt.y, maxPt.z);

        addTransformedLine(shapeMatrix, p000, p100);
        addTransformedLine(shapeMatrix, p100, p110);
        addTransformedLine(shapeMatrix, p110, p010);
        addTransformedLine(shapeMatrix, p010, p000);

        addTransformedLine(shapeMatrix, p001, p101);
        addTransformedLine(shapeMatrix, p101, p111);
        addTransformedLine(shapeMatrix, p111, p011);
        addTransformedLine(shapeMatrix, p011, p001);

        addTransformedLine(shapeMatrix, p000, p001);
        addTransformedLine(shapeMatrix, p100, p101);
        addTransformedLine(shapeMatrix, p110, p111);
        addTransformedLine(shapeMatrix, p010, p011);
    };

    auto addBoxRings = [&](const Matrix4& shapeMatrix, const Vector3& minPt, const Vector3& maxPt) {
        const int ringCount = 3;
        for (int r = 1; r <= ringCount; r++) {
            float t = (float)r / (float)(ringCount + 1);
            float y = minPt.y + (maxPt.y - minPt.y) * t;

            Vector3 p0(minPt.x, y, minPt.z);
            Vector3 p1(maxPt.x, y, minPt.z);
            Vector3 p2(maxPt.x, y, maxPt.z);
            Vector3 p3(minPt.x, y, maxPt.z);

            addTransformedLine(shapeMatrix, p0, p1);
            addTransformedLine(shapeMatrix, p1, p2);
            addTransformedLine(shapeMatrix, p2, p3);
            addTransformedLine(shapeMatrix, p3, p0);
        }
    };

    auto addBoxVerticals = [&](const Matrix4& shapeMatrix, const Vector3& minPt, const Vector3& maxPt) {
        const int verticalCount = 3;

        for (int v = 1; v <= verticalCount; v++) {
            float t = (float)v / (float)(verticalCount + 1);

            float x = minPt.x + (maxPt.x - minPt.x) * t;
            float z = minPt.z + (maxPt.z - minPt.z) * t;

            addTransformedLine(shapeMatrix, Vector3(x, minPt.y, minPt.z), Vector3(x, maxPt.y, minPt.z));
            addTransformedLine(shapeMatrix, Vector3(x, minPt.y, maxPt.z), Vector3(x, maxPt.y, maxPt.z));

            addTransformedLine(shapeMatrix, Vector3(minPt.x, minPt.y, z), Vector3(minPt.x, maxPt.y, z));
            addTransformedLine(shapeMatrix, Vector3(maxPt.x, minPt.y, z), Vector3(maxPt.x, maxPt.y, z));
        }
    };

    auto addBoxCaps = [&](const Matrix4& shapeMatrix, const Vector3& minPt, const Vector3& maxPt) {
        const int capLineCount = 3;

        for (int c = 1; c <= capLineCount; c++) {
            float t = (float)c / (float)(capLineCount + 1);

            float x = minPt.x + (maxPt.x - minPt.x) * t;
            float z = minPt.z + (maxPt.z - minPt.z) * t;

            addTransformedLine(shapeMatrix, Vector3(x, minPt.y, minPt.z), Vector3(x, minPt.y, maxPt.z));
            addTransformedLine(shapeMatrix, Vector3(x, maxPt.y, minPt.z), Vector3(x, maxPt.y, maxPt.z));

            addTransformedLine(shapeMatrix, Vector3(minPt.x, minPt.y, z), Vector3(maxPt.x, minPt.y, z));
            addTransformedLine(shapeMatrix, Vector3(minPt.x, maxPt.y, z), Vector3(maxPt.x, maxPt.y, z));
        }
    };

    auto addCircle = [&](const Matrix4& shapeMatrix, const Vector3& center, const Vector3& axisA, const Vector3& axisB, float radius, int segments) {
        if (radius <= 0.0f) {
            return;
        }

        for (int i = 0; i < segments; i++) {
            float a0 = (2.0f * M_PI * i) / segments;
            float a1 = (2.0f * M_PI * (i + 1)) / segments;

            Vector3 p0 = center + axisA * (std::cos(a0) * radius) + axisB * (std::sin(a0) * radius);
            Vector3 p1 = center + axisA * (std::cos(a1) * radius) + axisB * (std::sin(a1) * radius);

            addTransformedLine(shapeMatrix, p0, p1);
        }
    };

    auto addHemisphere = [&](const Matrix4& shapeMatrix, const Vector3& center, float radius, bool topHemisphere) {
        if (radius <= 0.0f) {
            return;
        }

        const int elevationSegments = 6;
        const int azimuthSegments = 12;
        const float ySign = topHemisphere ? 1.0f : -1.0f;

        for (int a = 0; a < azimuthSegments; a++) {
            float az = (2.0f * M_PI * a) / azimuthSegments;
            Vector3 dirXZ(std::cos(az), 0.0f, std::sin(az));

            for (int e = 0; e < elevationSegments; e++) {
                float phi0 = (0.5f * M_PI * e) / elevationSegments;
                float phi1 = (0.5f * M_PI * (e + 1)) / elevationSegments;

                Vector3 p0 = center + dirXZ * (std::cos(phi0) * radius) + Vector3(0.0f, ySign * std::sin(phi0) * radius, 0.0f);
                Vector3 p1 = center + dirXZ * (std::cos(phi1) * radius) + Vector3(0.0f, ySign * std::sin(phi1) * radius, 0.0f);

                addTransformedLine(shapeMatrix, p0, p1);
            }
        }

        const int ringCount = 3;
        for (int r = 1; r <= ringCount; r++) {
            float phi = (0.5f * M_PI * r) / (ringCount + 1);
            float ringRadius = std::cos(phi) * radius;
            float y = ySign * std::sin(phi) * radius;

            addCircle(shapeMatrix, center + Vector3(0.0f, y, 0.0f), Vector3(1, 0, 0), Vector3(0, 0, 1), ringRadius, 18);
        }
    };

    auto addCapsuleMiddle = [&](const Matrix4& shapeMatrix, float halfHeight, float topRadius, float bottomRadius) {
        const int azimuthSegments = 12;
        for (int a = 0; a < azimuthSegments; a++) {
            float az = (2.0f * M_PI * a) / azimuthSegments;
            Vector3 dirXZ(std::cos(az), 0.0f, std::sin(az));

            Vector3 topPoint = Vector3(0.0f, halfHeight, 0.0f) + dirXZ * topRadius;
            Vector3 bottomPoint = Vector3(0.0f, -halfHeight, 0.0f) + dirXZ * bottomRadius;
            addTransformedLine(shapeMatrix, topPoint, bottomPoint);
        }
    };

    auto addCylinderRings = [&](const Matrix4& shapeMatrix, float halfHeight, float radius) {
        const int ringCount = 3;
        for (int r = 1; r <= ringCount; r++) {
            float t = (float)r / (float)(ringCount + 1);
            float y = halfHeight - (2.0f * halfHeight * t);
            addCircle(shapeMatrix, Vector3(0.0f, y, 0.0f), Vector3(1, 0, 0), Vector3(0, 0, 1), radius, 18);
        }
    };

    auto addSpherePattern = [&](const Matrix4& shapeMatrix, float radius) {
        if (radius <= 0.0f) {
            return;
        }

        const int azimuthSegments = 12;
        const int elevationSegments = 12;

        for (int a = 0; a < azimuthSegments; a++) {
            float az = (2.0f * M_PI * a) / azimuthSegments;
            Vector3 dirXZ(std::cos(az), 0.0f, std::sin(az));

            for (int e = 0; e < elevationSegments; e++) {
                float phi0 = -0.5f * M_PI + (M_PI * e) / elevationSegments;
                float phi1 = -0.5f * M_PI + (M_PI * (e + 1)) / elevationSegments;

                Vector3 p0 = dirXZ * (std::cos(phi0) * radius) + Vector3(0.0f, std::sin(phi0) * radius, 0.0f);
                Vector3 p1 = dirXZ * (std::cos(phi1) * radius) + Vector3(0.0f, std::sin(phi1) * radius, 0.0f);

                addTransformedLine(shapeMatrix, p0, p1);
            }
        }

        const int latitudeRings = 5;
        for (int r = 1; r <= latitudeRings; r++) {
            float phi = -0.5f * M_PI + (M_PI * r) / (latitudeRings + 1);
            float ringRadius = std::cos(phi) * radius;
            float y = std::sin(phi) * radius;

            addCircle(shapeMatrix, Vector3(0.0f, y, 0.0f), Vector3(1, 0, 0), Vector3(0, 0, 1), ringRadius, 20);
        }
    };

    // An empty name keeps the buffer already in view; an unknown one returns null so the
    // attribute is dropped, since its offset only means anything against its own buffer.
    auto resolveNamedBuffer = [](const std::map<std::string, Buffer*>& buffers, const std::string& bufferName, Buffer* current) -> Buffer* {
        if (bufferName.empty()) {
            return current;
        }

        auto it = buffers.find(bufferName);
        return it != buffers.end() ? it->second : nullptr;
    };

    // A multi-node glTF root holds no geometry of its own, so a shape sourced from it
    // reads the model's mesh-node children instead.
    auto shapeSourceEntities = [&](const Collider3D& shapeData) {
        std::vector<Entity> sources;
        Entity meshEntity = shapeData.sourceEntity == NULL_ENTITY ? entity : shapeData.sourceEntity;

        MeshComponent* mesh = scene->findComponent<MeshComponent>(meshEntity);
        if (mesh && mesh->numSubmeshes > 0) {
            sources.push_back(meshEntity);
            return sources;
        }

        if (ModelComponent* model = scene->findComponent<ModelComponent>(meshEntity)) {
            for (auto const& node : model->meshNodesMapping) {
                MeshComponent* nodeMesh = scene->findComponent<MeshComponent>(node.second);
                if (nodeMesh && nodeMesh->numSubmeshes > 0) {
                    sources.push_back(node.second);
                }
            }
        }

        return sources;
    };

    // Source geometry through its world matrix and back into the body's local
    // (rotation-free, world-scaled) space, the same mapping PhysicsSystem applies.
    auto sourceToBodyMatrix = [&](Entity sourceEntity) -> Matrix4 {
        Transform* sourceTransform = scene->findComponent<Transform>(sourceEntity);
        if (sourceEntity == entity || !sourceTransform) {
            return Matrix4::scaleMatrix(sourceTransform ? sourceTransform->worldScale : Vector3::UNIT_SCALE);
        }

        return transform.worldRotation.inverse().getRotationMatrix() *
               Matrix4::translateMatrix(-transform.worldPosition) *
               sourceTransform->modelMatrix;
    };

    auto forEachSourceSubmesh = [&](const std::vector<Entity>& sources, auto&& callback) {
        for (Entity sourceEntity : sources) {
            MeshComponent* mesh = scene->findComponent<MeshComponent>(sourceEntity);
            if (!mesh) continue;
            ModelCpuData::ensure(*mesh);

            std::map<std::string, Buffer*> buffers;
            if (mesh->buffer.getSize() > 0) buffers["vertices"] = &mesh->buffer;
            if (mesh->indices.getSize() > 0) buffers["indices"] = &mesh->indices;
            for (int bufferIndex = 0; bufferIndex < mesh->numExternalBuffers; bufferIndex++) {
                buffers[mesh->eBuffers[bufferIndex].getName()] = &mesh->eBuffers[bufferIndex];
            }

            Buffer* indexBuffer = nullptr;
            Attribute indexAttr;
            Buffer* vertexBuffer = nullptr;
            Attribute vertexAttr;

            for (auto const& buf : buffers) {
                if (buf.second->getAttribute(AttributeType::INDEX)) {
                    indexBuffer = buf.second;
                    indexAttr = *buf.second->getAttribute(AttributeType::INDEX);
                }
                if (buf.second->getAttribute(AttributeType::POSITION)) {
                    vertexBuffer = buf.second;
                    vertexAttr = *buf.second->getAttribute(AttributeType::POSITION);
                }
            }

            const Matrix4 toBody = sourceToBodyMatrix(sourceEntity);

            for (size_t submeshIndex = 0; submeshIndex < mesh->numSubmeshes; submeshIndex++) {
                for (auto const& attr : mesh->submeshes[submeshIndex].attributes) {
                    if (attr.first == AttributeType::INDEX) {
                        if (Buffer* resolved = resolveNamedBuffer(buffers, attr.second.getBufferName(), indexBuffer)) {
                            indexBuffer = resolved;
                            indexAttr = attr.second;
                        }
                    }
                    if (attr.first == AttributeType::POSITION) {
                        if (Buffer* resolved = resolveNamedBuffer(buffers, attr.second.getBufferName(), vertexBuffer)) {
                            vertexBuffer = resolved;
                            vertexAttr = attr.second;
                        }
                    }
                }

                callback(vertexBuffer, vertexAttr, indexBuffer, indexAttr, toBody);
            }
        }
    };

    // Placement comes from the local chain up to the body, not from the mapping matrix:
    // the two agree, but the matrix jitters in its low bits when the body itself moves.
    auto sourceSignature = [&](const Collider3D& shapeData, const std::vector<Entity>& sources) {
        uint64_t hash = 1469598103934665603ull;
        auto mix = [&hash](const void* data, size_t size) {
            const unsigned char* bytes = static_cast<const unsigned char*>(data);
            for (size_t byteIndex = 0; byteIndex < size; byteIndex++) {
                hash ^= bytes[byteIndex];
                hash *= 1099511628211ull;
            }
        };

        // One slot serves every shape type at this index
        int shapeType = int(shapeData.type);
        int shapeSource = int(shapeData.source);
        mix(&shapeType, sizeof(shapeType));
        mix(&shapeSource, sizeof(shapeSource));
        mix(&pose.scale, sizeof(Vector3));

        for (Entity sourceEntity : sources) {
            Entity current = sourceEntity;
            size_t hops = 0;

            while (current != NULL_ENTITY && current != entity && hops <= kMaxSourceParentHops) {
                Transform* sourceTransform = scene->findComponent<Transform>(current);
                if (!sourceTransform) break;

                mix(&sourceTransform->position, sizeof(Vector3));
                mix(&sourceTransform->rotation, sizeof(Quaternion));
                mix(&sourceTransform->scale, sizeof(Vector3));

                current = sourceTransform->parent;
                hops++;
            }

            // Outside the body's subtree there is no chain to walk, so quantize instead
            if (current != entity) {
                const Matrix4 toBody = sourceToBodyMatrix(sourceEntity);
                for (int element = 0; element < 16; element++) {
                    int64_t quantized = std::llround(double(static_cast<const float*>(toBody)[element]) * kHullQuantizeScale);
                    mix(&quantized, sizeof(quantized));
                }
            }
        }

        forEachSourceSubmesh(sources, [&](Buffer* vbuf, Attribute vattr, Buffer* ibuf, Attribute iattr, const Matrix4&) {
            const void* vdata = vbuf ? static_cast<const void*>(vbuf->getData()) : nullptr;
            const void* idata = ibuf ? static_cast<const void*>(ibuf->getData()) : nullptr;
            size_t vertexCount = vbuf ? vattr.getCount() : 0;
            size_t indexCount = ibuf ? iattr.getCount() : 0;

            mix(&vdata, sizeof(vdata));
            mix(&idata, sizeof(idata));
            mix(&vertexCount, sizeof(vertexCount));
            mix(&indexCount, sizeof(indexCount));
        });

        return hash;
    };

    auto shapeEdgeCache = [&](size_t index) -> EdgeCache& {
        if (caches.size() != body.colliders.size()) {
            caches.assign(body.colliders.size(), EdgeCache{});
        }
        return caches[index];
    };

    for (size_t i = 0; i < body.colliders.size(); i++) {
        const Collider3D& shapeData = body.colliders[i];
        colliderIndex = i;

        Matrix4 shapeMatrix = Matrix4::translateMatrix(shapeData.position * entityScale) * shapeData.rotation.getRotationMatrix();

        if (shapeData.type == Collider3DType::BOX) {
            Vector3 halfSize(shapeData.width * absScale.x * 0.5f, shapeData.height * absScale.y * 0.5f, shapeData.depth * absScale.z * 0.5f);
            addBox(shapeMatrix, -halfSize, halfSize);
            addBoxRings(shapeMatrix, -halfSize, halfSize);
            addBoxVerticals(shapeMatrix, -halfSize, halfSize);
            addBoxCaps(shapeMatrix, -halfSize, halfSize);
        } else if (shapeData.type == Collider3DType::SPHERE) {
            addSpherePattern(shapeMatrix, shapeData.radius * maxScaleXYZ());
        } else if (shapeData.type == Collider3DType::CAPSULE) {
            const float halfHeight = shapeData.halfHeight * absScale.y;
            const float radius = shapeData.radius * maxScaleXZ();
            Vector3 top(0, halfHeight, 0);
            Vector3 bottom(0, -halfHeight, 0);

            addCircle(shapeMatrix, top, Vector3(1, 0, 0), Vector3(0, 0, 1), radius, 20);
            addCircle(shapeMatrix, bottom, Vector3(1, 0, 0), Vector3(0, 0, 1), radius, 20);
            addCapsuleMiddle(shapeMatrix, halfHeight, radius, radius);

            addHemisphere(shapeMatrix, top, radius, true);
            addHemisphere(shapeMatrix, bottom, radius, false);
        } else if (shapeData.type == Collider3DType::TAPERED_CAPSULE) {
            const float halfHeight = shapeData.halfHeight * absScale.y;
            const float radiusScale = maxScaleXZ();
            const float topRadius = shapeData.topRadius * radiusScale;
            const float bottomRadius = shapeData.bottomRadius * radiusScale;

            addCircle(shapeMatrix, Vector3(0, halfHeight, 0), Vector3(1, 0, 0), Vector3(0, 0, 1), topRadius, 20);
            addCircle(shapeMatrix, Vector3(0, -halfHeight, 0), Vector3(1, 0, 0), Vector3(0, 0, 1), bottomRadius, 20);
            addCapsuleMiddle(shapeMatrix, halfHeight, topRadius, bottomRadius);

            addHemisphere(shapeMatrix, Vector3(0, halfHeight, 0), topRadius, true);
            addHemisphere(shapeMatrix, Vector3(0, -halfHeight, 0), bottomRadius, false);
        } else if (shapeData.type == Collider3DType::CYLINDER) {
            const float halfHeight = shapeData.halfHeight * absScale.y;
            const float radius = shapeData.radius * maxScaleXZ();

            addCircle(shapeMatrix, Vector3(0, halfHeight, 0), Vector3(1, 0, 0), Vector3(0, 0, 1), radius, 20);
            addCircle(shapeMatrix, Vector3(0, -halfHeight, 0), Vector3(1, 0, 0), Vector3(0, 0, 1), radius, 20);
            addCapsuleMiddle(shapeMatrix, halfHeight, radius, radius);
            addCylinderRings(shapeMatrix, halfHeight, radius);
        } else if (shapeData.type == Collider3DType::CONVEX_HULL) {
            if (shapeData.source == Collider3DSource::ENTITY_MESH) {
                // Walking every source vertex and rebuilding the hull is far too heavy
                // to repeat per frame
                EdgeCache& cache = shapeEdgeCache(i);
                std::vector<Entity> sources = shapeSourceEntities(shapeData);
                uint64_t signature = sourceSignature(shapeData, sources);

                if (!cache.valid || cache.signature != signature) {
                    cache.valid = true;
                    cache.signature = signature;
                    cache.edges.clear();

                    std::vector<Vector3> hullPoints;
                    std::unordered_set<uint64_t> seen;
                    forEachSourceSubmesh(sources, [&](Buffer* vbuf, Attribute vattr, Buffer*, Attribute, const Matrix4& toBody) {
                        if (!vbuf) return;
                        int count = int(vattr.getCount());
                        for (int v = 0; v < count; v++) {
                            pushUniqueHullPoint(hullPoints, seen, toBody * vbuf->getVector3(&vattr, v));
                        }
                    });

                    buildConvexHullEdges(hullPoints, [&](const Vector3& a, const Vector3& b) {
                        cache.edges.emplace_back(a, b);
                    });
                }

                for (const auto& e : cache.edges) {
                    addTransformedLine(shapeMatrix, e.first, e.second);
                }
            } else if (shapeData.vertices.size() >= 2) {
                // Raw vertices are reduced to their extremes past kMaxHullInputPoints, so build them inline.
                std::vector<Vector3> hullPoints;
                std::unordered_set<uint64_t> seen;
                hullPoints.reserve(shapeData.vertices.size());
                for (size_t vertexIndex = 0; vertexIndex < shapeData.vertices.size(); vertexIndex++) {
                    pushUniqueHullPoint(hullPoints, seen, shapeData.vertices[vertexIndex] * entityScale);
                }

                buildConvexHullEdges(hullPoints, [&](const Vector3& a, const Vector3& b) {
                    addTransformedLine(shapeMatrix, a, b);
                });
            }
        } else if (shapeData.type == Collider3DType::MESH) {
            if (shapeData.source == Collider3DSource::ENTITY_MESH) {
                // Cached in body-local space, so a frame only costs two matrix
                // transforms + addLine per edge
                EdgeCache& cache = shapeEdgeCache(i);
                std::vector<Entity> sources = shapeSourceEntities(shapeData);
                uint64_t signature = sourceSignature(shapeData, sources);

                if (!cache.valid || cache.signature != signature) {
                    cache.valid = true;
                    cache.signature = signature;
                    cache.edges.clear();

                    // Interior edges belong to 2 triangles, so deduping by index pair
                    // halves the lines. Per submesh: indices are local to their own.
                    std::unordered_set<uint64_t> seenEdges;
                    forEachSourceSubmesh(sources, [&](Buffer* vbuf, Attribute vattr, Buffer* ibuf, Attribute iattr, const Matrix4& toBody) {
                        seenEdges.clear();
                        if (!vbuf) return;
                        uint32_t vcount = uint32_t(vattr.getCount());
                        auto pushEdge = [&](uint32_t a, uint32_t b) {
                            if (a == b || a >= vcount || b >= vcount) return;
                            uint32_t lo = std::min(a, b);
                            uint32_t hi = std::max(a, b);
                            uint64_t key = (uint64_t(lo) << 32) | hi;
                            if (!seenEdges.insert(key).second) return;
                            cache.edges.emplace_back(toBody * vbuf->getVector3(&vattr, lo), toBody * vbuf->getVector3(&vattr, hi));
                        };

                        if (!ibuf) {
                            uint32_t triCount = vcount / 3;
                            for (uint32_t t = 0; t < triCount; t++) {
                                uint32_t a = t * 3, b = a + 1, c = a + 2;
                                pushEdge(a, b);
                                pushEdge(b, c);
                                pushEdge(c, a);
                            }
                            return;
                        }
                        uint32_t triCount = uint32_t(iattr.getCount() / 3);
                        auto readIndex = [&](uint32_t idx) -> uint32_t {
                            if (iattr.getDataType() == AttributeDataType::UNSIGNED_INT) return ibuf->getUInt32(&iattr, idx);
                            if (iattr.getDataType() == AttributeDataType::UNSIGNED_SHORT) return ibuf->getUInt16(&iattr, idx);
                            return UINT32_MAX;
                        };
                        for (uint32_t t = 0; t < triCount; t++) {
                            uint32_t i0 = readIndex(t * 3);
                            uint32_t i1 = readIndex(t * 3 + 1);
                            uint32_t i2 = readIndex(t * 3 + 2);
                            if (i0 == UINT32_MAX || i1 == UINT32_MAX || i2 == UINT32_MAX) continue;
                            pushEdge(i0, i1);
                            pushEdge(i1, i2);
                            pushEdge(i2, i0);
                        }
                    });
                }

                for (const auto& e : cache.edges) {
                    addTransformedLine(shapeMatrix, e.first, e.second);
                }
            } else if (shapeData.vertices.size() >= 3 && shapeData.indices.size() >= 3) {
                const size_t vertexCount = shapeData.vertices.size();
                const size_t triangles = shapeData.indices.size() / 3;
                for (size_t tri = 0; tri < triangles; tri++) {
                    uint32_t i0 = shapeData.indices[tri * 3 + 0];
                    uint32_t i1 = shapeData.indices[tri * 3 + 1];
                    uint32_t i2 = shapeData.indices[tri * 3 + 2];

                    if (i0 < vertexCount && i1 < vertexCount && i2 < vertexCount) {
                        Vector3 p0 = shapeData.vertices[i0] * entityScale;
                        Vector3 p1 = shapeData.vertices[i1] * entityScale;
                        Vector3 p2 = shapeData.vertices[i2] * entityScale;

                        addTransformedLine(shapeMatrix, p0, p1);
                        addTransformedLine(shapeMatrix, p1, p2);
                        addTransformedLine(shapeMatrix, p2, p0);
                    }
                }
            }
        } else if (shapeData.type == Collider3DType::HEIGHTFIELD) {
            Entity terrainEntity = shapeData.sourceEntity == NULL_ENTITY ? entity : shapeData.sourceEntity;
            TerrainComponent* terrain = scene->findComponent<TerrainComponent>(terrainEntity);
            if (terrain) {
                Vector3 minPt(-terrain->terrainSize * 0.5f, 0.0f, -terrain->terrainSize * 0.5f);
                Vector3 maxPt(terrain->terrainSize * 0.5f, terrain->maxHeight, terrain->terrainSize * 0.5f);
                addBox(shapeMatrix, minPt, maxPt);
            }
        }
    }
}

void PhysicsDebugLines::crossMark(const Vector3& center, float size, const Emit& emit){
    emit(center + Vector3(-size, 0.0f, 0.0f), center + Vector3(size, 0.0f, 0.0f));
    emit(center + Vector3(0.0f, -size, 0.0f), center + Vector3(0.0f, size, 0.0f));
    emit(center + Vector3(0.0f, 0.0f, -size), center + Vector3(0.0f, 0.0f, size));
}

namespace {

    // an orthonormal basis (n, right, forward) around a direction
    void basisOf(const Vector3& axis, Vector3& n, Vector3& right, Vector3& forward){
        n = axis.length() > 0.0001f ? axis.normalized() : Vector3(1.0f, 0.0f, 0.0f);
        Vector3 up(0.0f, 1.0f, 0.0f);
        if (std::fabs(n.dotProduct(up)) > 0.9f){
            up = Vector3(1.0f, 0.0f, 0.0f);
        }
        right = n.crossProduct(up).normalized();
        forward = right.crossProduct(n).normalized();
    }

    Vector3 rotated(const PhysicsDebugLines::Pose& pose, const Vector3& v){
        return pose.rotation * v;
    }

}

void PhysicsDebugLines::jointAnchors(const Joint3DComponent& joint, const std::function<bool(Entity, Pose&)>& poseOf,
                                     Vector3& anchorA, Vector3& anchorB){
    Pose poseA, poseB;
    const bool hasA = joint.bodyA != NULL_ENTITY && poseOf(joint.bodyA, poseA);
    const bool hasB = joint.bodyB != NULL_ENTITY && poseOf(joint.bodyB, poseB);

    if (joint.anchorSpace == JointAnchorSpace::LOCAL){
        anchorA = hasA ? poseA.position + poseA.rotation * (joint.anchor * poseA.scale) : joint.anchor;
        if (joint.autoAnchors && joint.type != Joint3DType::DISTANCE){
            anchorB = anchorA;
        }else if (hasB){
            anchorB = poseB.position + poseB.rotation * (joint.anchorB * poseB.scale);
        }else{
            // held to the world: a world point
            anchorB = joint.anchorB;
        }
        return;
    }

    anchorA = hasA ? poseA.position : Vector3::ZERO;
    anchorB = hasB ? poseB.position : Vector3::ZERO;
    switch (joint.type){
        case Joint3DType::DISTANCE:
            if (!joint.autoAnchors){
                anchorA = joint.anchorA;
                anchorB = joint.anchorB;
            }
            break;
        case Joint3DType::POINT:
        case Joint3DType::HINGE:
        case Joint3DType::CONE:
        case Joint3DType::SWINGTWIST:
            anchorA = anchorB = joint.anchor;
            break;
        case Joint3DType::SIXDOF:
        case Joint3DType::PULLEY:
            anchorA = joint.anchorA;
            anchorB = joint.anchorB;
            break;
        case Joint3DType::PRISMATIC:
            anchorA = anchorB = (anchorA + anchorB) * 0.5f;
            break;
        default:
            break;
    }
}

void PhysicsDebugLines::jointEdges(Scene* scene, const Joint3DComponent& joint, const std::function<bool(Entity, Pose&)>& poseOf,
                                   const std::function<void(const Vector3&, const Vector3&, int)>& emit){
    (void)scene;
    const int JOINT = 0, HELPER = 1, AXIS = 2, LIMIT = 3;

    Vector3 anchorA, anchorB;
    jointAnchors(joint, poseOf, anchorA, anchorB);

    // LOCAL axes are bodyA's directions
    Pose poseA;
    const bool local = joint.anchorSpace == JointAnchorSpace::LOCAL && joint.bodyA != NULL_ENTITY && poseOf(joint.bodyA, poseA);
    auto direction = [&](const Vector3& axis){
        return local ? rotated(poseA, axis) : axis;
    };

    auto ring = [&](const Vector3& center, const Vector3& axis, float radius, int kind, int segments){
        if (radius <= 0.0f) return;
        Vector3 n, right, forward;
        basisOf(axis, n, right, forward);
        for (int i = 0; i < segments; i++){
            const float a0 = (2.0f * (float)M_PI * i) / segments;
            const float a1 = (2.0f * (float)M_PI * (i + 1)) / segments;
            emit(center + right * std::cos(a0) * radius + forward * std::sin(a0) * radius,
                 center + right * std::cos(a1) * radius + forward * std::sin(a1) * radius, kind);
        }
    };
    // an arc about axis from `from` (perpendicular to it), angles in radians
    auto arc = [&](const Vector3& center, const Vector3& axis, const Vector3& from, float start, float end, float radius, int kind, int segments){
        if (radius <= 0.0f) return;
        Vector3 n = axis.length() > 0.0001f ? axis.normalized() : Vector3(0.0f, 1.0f, 0.0f);
        Vector3 base = from.length() > 0.0001f ? from.normalized() : Vector3(1.0f, 0.0f, 0.0f);
        if (std::fabs(base.dotProduct(n)) > 0.95f){
            Vector3 nn, right, forward;
            basisOf(n, nn, right, forward);
            base = right;
        }
        const Vector3 tangent = n.crossProduct(base).normalized();
        for (int i = 0; i < segments; i++){
            const float a0 = start + (end - start) * ((float)i / segments);
            const float a1 = start + (end - start) * ((float)(i + 1) / segments);
            emit(center + (base * std::cos(a0) + tangent * std::sin(a0)) * radius,
                 center + (base * std::cos(a1) + tangent * std::sin(a1)) * radius, kind);
        }
    };
    auto unitOr = [](const Vector3& v, const Vector3& fallback){
        return v.length() > 0.0001f ? v.normalized() : fallback;
    };

    emit(anchorA, anchorB, JOINT);
    const float markSize = 0.12f;
    crossMark(anchorA, markSize, [&](const Vector3& a, const Vector3& b){ emit(a, b, HELPER); });
    crossMark(anchorB, markSize, [&](const Vector3& a, const Vector3& b){ emit(a, b, HELPER); });

    const Vector3 dirAB = anchorB - anchorA;
    const float lenAB = dirAB.length();
    const Vector3 dirABNorm = lenAB > 0.0001f ? dirAB / lenAB : Vector3(1.0f, 0.0f, 0.0f);

    switch (joint.type){
        case Joint3DType::HINGE:{
            const Vector3 axis = joint.axis.length() > 0.0001f ? unitOr(direction(joint.axis), dirABNorm) : dirABNorm;
            const Vector3 center = (anchorA + anchorB) * 0.5f;
            ring(center, axis, 0.25f, AXIS, 28);
            emit(center - axis * 0.35f, center + axis * 0.35f, AXIS);
            if (joint.useLimits){
                // the limits measured from the pose the joint starts in, about the axis from
                // the normal
                Vector3 normal = direction(joint.normal);
                normal = normal - axis * normal.dotProduct(axis);
                float minRad = Angle::defaultToRad(joint.limitsMin);
                float maxRad = Angle::defaultToRad(joint.limitsMax);
                if (maxRad < minRad) std::swap(minRad, maxRad);
                if (joint.anchorSpace == JointAnchorSpace::WORLD){
                    // a world joint's limits are Jolt's: bodyB's angle, bodyA turns the other way
                    const float low = minRad;
                    minRad = -maxRad;
                    maxRad = -low;
                }
                arc(center, axis, normal, minRad, maxRad, 0.4f, LIMIT, 20);
                Vector3 n = normal.length() > 0.0001f ? normal.normalized() : Vector3::ZERO;
                if (n == Vector3::ZERO){
                    Vector3 nn, right, forward;
                    basisOf(axis, nn, right, forward);
                    n = right;
                }
                const Vector3 tangent = axis.crossProduct(n).normalized();
                emit(center, center + (n * std::cos(minRad) + tangent * std::sin(minRad)) * 0.4f, LIMIT);
                emit(center, center + (n * std::cos(maxRad) + tangent * std::sin(maxRad)) * 0.4f, LIMIT);
            }else{
                ring(center, axis, 0.33f, HELPER, 28);
            }
            break;
        }
        case Joint3DType::PRISMATIC:{
            const Vector3 railAxis = joint.axis.length() > 0.0001f ? unitOr(direction(joint.axis), dirABNorm) : dirABNorm;
            Vector3 n, side, up;
            basisOf(railAxis, n, side, up);
            float limitMin = joint.limitsMin;
            float limitMax = joint.limitsMax;
            if (limitMax < limitMin){
                std::swap(limitMin, limitMax);
            }
            if (!local){
                // WORLD joints start at 0 inside their (normalised) range
                if (limitMin > 0.0f){
                    const float offset = limitMin;
                    limitMin -= offset;
                    limitMax -= offset;
                }else if (limitMax < 0.0f){
                    const float offset = limitMax;
                    limitMin -= offset;
                    limitMax -= offset;
                }
            }
            if (!joint.useLimits || std::fabs(limitMax - limitMin) < 0.000001f){
                limitMin = -0.4f;
                limitMax = 0.4f;
            }

            const Vector3 railStart = anchorA + railAxis * limitMin;
            const Vector3 railEnd = anchorA + railAxis * limitMax;
            const float railOffset = 0.06f;
            emit(railStart + side * railOffset, railEnd + side * railOffset, AXIS);
            emit(railStart - side * railOffset, railEnd - side * railOffset, AXIS);
            if (joint.useLimits){
                emit(railStart - side * 0.12f, railStart + side * 0.12f, LIMIT);
                emit(railEnd - side * 0.12f, railEnd + side * 0.12f, LIMIT);
            }
            ring(anchorA, railAxis, 0.09f, JOINT, 18);
            emit(anchorA - up * 0.1f, anchorA + up * 0.1f, HELPER);
            break;
        }
        case Joint3DType::CONE:{
            const Vector3 axis = joint.twistAxis.length() > 0.0001f ? unitOr(direction(joint.twistAxis), dirABNorm) : dirABNorm;
            Vector3 coneAxis, right, forward;
            basisOf(axis, coneAxis, right, forward);
            const float coneLen = 0.9f;
            const float angle = std::clamp(Angle::defaultToRad(joint.coneHalfAngle), 0.0f, (float)M_PI * 0.499f);
            const float radius = coneLen * std::tan(angle);
            const Vector3 end = anchorA + coneAxis * coneLen;
            ring(end, coneAxis, radius, LIMIT, 22);
            emit(anchorA, end + forward * radius, HELPER);
            emit(anchorA, end - forward * radius, HELPER);
            emit(anchorA, end + right * radius, HELPER);
            emit(anchorA, end - right * radius, HELPER);
            break;
        }
        case Joint3DType::SWINGTWIST:{
            const Vector3 axis = joint.twistAxis.length() > 0.0001f ? unitOr(direction(joint.twistAxis), dirABNorm)
                                 : (joint.axis.length() > 0.0001f ? unitOr(direction(joint.axis), dirABNorm) : dirABNorm);
            const Vector3 center = (anchorA + anchorB) * 0.5f;
            Vector3 n, right, forward;
            basisOf(axis, n, right, forward);
            // the plane axis (Unity swingAxis) and the normal: swing1 about the first, swing2
            // about the second
            Vector3 planeAxis = direction(joint.planeAxis);
            planeAxis = planeAxis - n * planeAxis.dotProduct(n);
            if (planeAxis.length() > 0.0001f){
                right = planeAxis.normalized();
                forward = n.crossProduct(right).normalized();
            }
            const float twistMin = Angle::defaultToRad(std::min(joint.twistMinAngle, joint.twistMaxAngle));
            const float twistMax = Angle::defaultToRad(std::max(joint.twistMinAngle, joint.twistMaxAngle));
            arc(center, n, forward, twistMin, twistMax, 0.36f, LIMIT, 16);
            emit(center, center + forward * 0.36f, HELPER);

            // the swing cone: an ellipse at 0.9 along the twist axis
            const float coneLen = 0.9f;
            const float swing1 = std::clamp(Angle::defaultToRad(joint.planeHalfConeAngle), 0.0f, (float)M_PI * 0.45f);
            const float swing2 = std::clamp(Angle::defaultToRad(joint.normalHalfConeAngle), 0.0f, (float)M_PI * 0.45f);
            // swinging about the plane axis tilts the twist axis towards the normal (forward)
            const float alongForward = coneLen * std::tan(swing1);
            const float alongRight = coneLen * std::tan(swing2);
            const Vector3 end = center + n * coneLen;
            const int segments = 24;
            for (int i = 0; i < segments; i++){
                const float a0 = (2.0f * (float)M_PI * i) / segments;
                const float a1 = (2.0f * (float)M_PI * (i + 1)) / segments;
                emit(end + forward * (std::cos(a0) * alongForward) + right * (std::sin(a0) * alongRight),
                     end + forward * (std::cos(a1) * alongForward) + right * (std::sin(a1) * alongRight), LIMIT);
            }
            emit(center, end + forward * alongForward, HELPER);
            emit(center, end - forward * alongForward, HELPER);
            emit(center, end + right * alongRight, HELPER);
            emit(center, end - right * alongRight, HELPER);
            emit(center, end, AXIS);
            break;
        }
        case Joint3DType::SIXDOF:{
            const Vector3 axisX = unitOr(direction(joint.axisX), Vector3(1.0f, 0.0f, 0.0f));
            Vector3 axisY = direction(joint.axisY);
            axisY = axisY - axisX * axisY.dotProduct(axisX);
            axisY = unitOr(axisY, Vector3(0.0f, 1.0f, 0.0f));
            const Vector3 axisZ = axisX.crossProduct(axisY);
            emit(anchorA, anchorA + axisX * 0.4f, AXIS);
            emit(anchorA, anchorA + axisY * 0.4f, AXIS);
            emit(anchorA, anchorA + axisZ * 0.4f, AXIS);
            break;
        }
        case Joint3DType::PATH:{
            if (joint.pathPoints.size() >= 2){
                for (size_t i = 0; i + 1 < joint.pathPoints.size(); i++){
                    emit(joint.pathPoints[i], joint.pathPoints[i + 1], AXIS);
                }
                if (joint.isLooping){
                    emit(joint.pathPoints.back(), joint.pathPoints.front(), AXIS);
                }
                ring(joint.pathPosition, Vector3(0.0f, 1.0f, 0.0f), 0.08f, JOINT, 16);
            }
            break;
        }
        case Joint3DType::PULLEY:{
            emit(anchorA, joint.fixedPointA, AXIS);
            emit(joint.fixedPointA, joint.fixedPointB, AXIS);
            emit(joint.fixedPointB, anchorB, AXIS);
            break;
        }
        default:
            break;
    }

    // Only draw generic axis for types that don't already visualize it
    if (joint.axis.length() > 0.0001f && joint.type != Joint3DType::HINGE && joint.type != Joint3DType::PRISMATIC &&
            joint.type != Joint3DType::CONE && joint.type != Joint3DType::SWINGTWIST){
        const Vector3 axis = unitOr(direction(joint.axis), Vector3(1.0f, 0.0f, 0.0f));
        const Vector3 center = (anchorA + anchorB) * 0.5f;
        emit(center, center + axis, AXIS);
    }
}
