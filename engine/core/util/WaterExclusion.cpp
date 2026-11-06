// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "WaterExclusion.h"
#include "util/MeshTriangles.h"

#ifdef JASPERENGINE_PHYSICS_3D
#include "physics/JoltShapeBuilder.h"
#include "Jolt/Jolt.h"
#include "Jolt/Geometry/ConvexHullBuilder.h"
#endif

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <unordered_set>

using namespace jasperengine;

namespace{

    // snapping the hull's input: cells per side of its bounds
    constexpr int WATER_HULL_GRID = 1024;

    uint8_t waterHullUnorm8(float value){
        return (uint8_t)std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f);
    }

}

const char* jasperengine::getWaterHullStatusName(WaterHullStatus status){
    switch (status){
        case WaterHullStatus::NONE:       return "none";
        case WaterHullStatus::PENDING:    return "pending";
        case WaterHullStatus::READY:      return "ready";
        case WaterHullStatus::NO_MESH:    return "no_mesh";
        case WaterHullStatus::FLAT:       return "flat";
        case WaterHullStatus::NO_PHYSICS: return "no_physics";
    }
    return "none";
}

WaterHullStatus jasperengine::buildWaterHull(const std::vector<Vector3>& points, WaterHull& hull){
    hull = WaterHull();
    hull.sourceVertices = points.size();
    for (const Vector3& point : points){
        if (std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z))
            hull.bounds.merge(point);
    }
    if (hull.bounds.isNull())
        return WaterHullStatus::NO_MESH;

    const Vector3 size = hull.bounds.getSize();
    if (size.x <= 0.0f || size.y <= 0.0f || size.z <= 0.0f)
        return WaterHullStatus::FLAT;

#ifdef JASPERENGINE_PHYSICS_3D
    JoltShapeBuilder::ensureJoltGlobalInit();

    // the cells the points occupy, once each
    const Vector3 minimum = hull.bounds.getMinimum();
    const Vector3 cell = size / (float)WATER_HULL_GRID;
    std::unordered_set<uint64_t> cells;
    JPH::Array<JPH::Vec3> positions;
    for (const Vector3& point : points){
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z))
            continue;
        const uint64_t ix = (uint64_t)std::clamp((int)std::lround((point.x - minimum.x) / cell.x), 0, WATER_HULL_GRID);
        const uint64_t iy = (uint64_t)std::clamp((int)std::lround((point.y - minimum.y) / cell.y), 0, WATER_HULL_GRID);
        const uint64_t iz = (uint64_t)std::clamp((int)std::lround((point.z - minimum.z) / cell.z), 0, WATER_HULL_GRID);
        if (!cells.insert((ix << 22) | (iy << 11) | iz).second)
            continue;
        positions.push_back(JPH::Vec3(minimum.x + cell.x * (float)ix, minimum.y + cell.y * (float)iy, minimum.z + cell.z * (float)iz));
    }

    // vertices nearer the hull than a ten-thousandth of its size are left out
    JPH::ConvexHullBuilder builder(positions);
    const char* error = nullptr;
    const JPH::ConvexHullBuilder::EResult result = builder.Initialize(WATER_EXCLUDER_HULL_MAX_VERTICES,
                                                                      std::max(size.length() * 1.0e-4f, 1.0e-6f), error);
    if (result != JPH::ConvexHullBuilder::EResult::Success && result != JPH::ConvexHullBuilder::EResult::MaxVerticesReached)
        return WaterHullStatus::FLAT;

    std::map<int, uint16_t> pointOf;
    std::set<std::pair<uint16_t, uint16_t>> edges;
    for (const JPH::ConvexHullBuilder::Face* face : builder.GetFaces()){
        if (face->mRemoved)
            continue;
        const JPH::Vec3 normal = face->mNormal.Normalized();
        hull.planes.push_back(Vector4(normal.GetX(), normal.GetY(), normal.GetZ(), normal.Dot(face->mCentroid)));

        const JPH::ConvexHullBuilder::Edge* edge = face->mFirstEdge;
        do{
            const int from = edge->mStartIdx;
            const int to = edge->mNextEdge->mStartIdx;
            uint16_t ends[2];
            int index = 0;
            for (int vertex : {from, to}){
                auto found = pointOf.find(vertex);
                if (found == pointOf.end()){
                    found = pointOf.emplace(vertex, (uint16_t)hull.points.size()).first;
                    const JPH::Vec3 position = positions[vertex];
                    hull.points.push_back(Vector3(position.GetX(), position.GetY(), position.GetZ()));
                }
                ends[index++] = found->second;
            }
            edges.insert({std::min(ends[0], ends[1]), std::max(ends[0], ends[1])});
            edge = edge->mNextEdge;
        }while (edge != face->mFirstEdge);
    }
    if (hull.planes.size() < 4)
        return WaterHullStatus::FLAT;

    hull.edges.assign(edges.begin(), edges.end());
    hull.bounds = AABB();
    for (const Vector3& point : hull.points)
        hull.bounds.merge(point);
    return WaterHullStatus::READY;
#else
    return WaterHullStatus::NO_PHYSICS;
#endif
}

void jasperengine::getWaterHullMapArea(const AABB& bounds, Vector3& origin, Vector3& extent){
    const float margin = 2.0f / (float)(WATER_EXCLUDER_HULL_MAP_SIZE - 4);
    const Vector3 size = bounds.getSize();
    origin = bounds.getMinimum() - Vector3(size.x * margin, 0.0f, size.z * margin);
    extent = Vector3(std::max(size.x * (1.0f + 2.0f * margin), 1.0e-6f), std::max(size.y, 1.0e-6f),
                     std::max(size.z * (1.0f + 2.0f * margin), 1.0e-6f));
}

void jasperengine::buildWaterHullMap(const WaterHull& hull, std::vector<uint8_t>& rgba){
    Vector3 origin;
    Vector3 extent;
    getWaterHullMapArea(hull.bounds, origin, extent);

    const int size = WATER_EXCLUDER_HULL_MAP_SIZE;
    // four texels each way of the walls
    const float wallRange = 4.0f * std::max(extent.x, extent.z) / (float)size;
    rgba.assign((size_t)size * size * 4, 0);
    for (int z = 0; z < size; z++){
        for (int x = 0; x < size; x++){
            const float px = origin.x + extent.x * ((float)x + 0.5f) / (float)size;
            const float pz = origin.z + extent.z * ((float)z + 0.5f) / (float)size;
            float bottom = std::numeric_limits<float>::lowest();
            float top = std::numeric_limits<float>::max();
            float wall = -wallRange;
            for (const Vector4& plane : hull.planes){
                const float rest = plane.w - plane.x * px - plane.z * pz;
                if (plane.y > 0.001f){
                    top = std::min(top, rest / plane.y);
                }else if (plane.y < -0.001f){
                    bottom = std::max(bottom, rest / plane.y);
                }else{
                    wall = std::max(wall, -rest / std::sqrt(plane.x * plane.x + plane.z * plane.z));
                }
            }

            // an empty column stays empty, which clamping each height could turn into [1, 1]
            uint8_t* texel = &rgba[((size_t)z * size + x) * 4];
            texel[0] = (bottom > top) ? 255 : waterHullUnorm8((bottom - origin.y) / extent.y);
            texel[1] = (bottom > top) ? 0 : waterHullUnorm8((top - origin.y) / extent.y);
            texel[2] = waterHullUnorm8(0.5f + wall / (2.0f * wallRange));
            texel[3] = 255;
        }
    }
}

AABB jasperengine::getWaterExcluderLocalBox(WaterExcluderShape shape, const Vector3& center, const Vector3& size, const WaterHull* hull){
    if (shape == WaterExcluderShape::CONVEX_HULL){
        if (!hull || hull->bounds.isNull())
            return AABB();
        const Vector3 extent = hull->bounds.getSize();
        if (extent.x <= 0.0f || extent.y <= 0.0f || extent.z <= 0.0f)
            return AABB();
        return hull->bounds;
    }
    const Vector3 half(std::fabs(size.x) * 0.5f, std::fabs(size.y) * 0.5f, std::fabs(size.z) * 0.5f);
    if (half.x <= 0.0f || half.y <= 0.0f || half.z <= 0.0f)
        return AABB();
    return AABB(center - half, center + half);
}

bool jasperengine::getWaterExcluderUnitSpace(const AABB& localBox, bool hull, const Matrix4& modelMatrix, Matrix4& worldToUnit){
    if (localBox.isNull() || std::fabs(MeshTriangles::linearDeterminant(modelMatrix)) < 1.0e-12f)
        return false;
    Vector3 origin = localBox.getMinimum();
    Vector3 extent = localBox.getSize();
    if (hull)
        getWaterHullMapArea(localBox, origin, extent);
    if (extent.x <= 0.0f || extent.y <= 0.0f || extent.z <= 0.0f)
        return false;
    worldToUnit = Matrix4::scaleMatrix(Vector3(1.0f / extent.x, 1.0f / extent.y, 1.0f / extent.z)) *
                  Matrix4::translateMatrix(-origin) * modelMatrix.inverse();
    return true;
}

bool jasperengine::isInsideWaterExcluder(WaterExcluderShape shape, const AABB& localBox, const WaterHull* hull,
                                         const Matrix4& modelMatrix, const Vector3& point){
    if (localBox.isNull() || std::fabs(MeshTriangles::linearDeterminant(modelMatrix)) < 1.0e-12f)
        return false;
    const Vector3 local = modelMatrix.inverse() * point;
    if (!localBox.contains(local))
        return false;

    if (shape == WaterExcluderShape::CONVEX_HULL){
        if (!hull || hull->planes.empty())
            return true;
        for (const Vector4& plane : hull->planes){
            if (plane.x * local.x + plane.y * local.y + plane.z * local.z > plane.w)
                return false;
        }
        return true;
    }
    if (shape == WaterExcluderShape::SPHERE){
        const Vector3 half = localBox.getHalfSize();
        const Vector3 d = local - localBox.getCenter();
        const Vector3 q(d.x / half.x, d.y / half.y, d.z / half.z);
        return q.dotProduct(q) <= 1.0f;
    }
    return true;
}
