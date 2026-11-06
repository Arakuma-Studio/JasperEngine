// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef WATER_EXCLUSION_H
#define WATER_EXCLUSION_H

#include "Export.h"
#include "component/WaterExcluderComponent.h"
#include "math/AABB.h"
#include "math/Matrix4.h"
#include "math/Vector3.h"
#include "math/Vector4.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace jasperengine{

    // The volumes of Water Excluders (component/WaterExcluderComponent.h), without a scene: the
    // shape tests the physics, the queries and the shader agree on, and the convex hull of a mesh
    // with its map (the hull seen from above, for the shader).

    // the side of a hull's map (texels; one layer of RenderSystem's array per hull)
    constexpr int WATER_EXCLUDER_HULL_MAP_SIZE = 128;
    // a hull keeps at most this many vertices (under 252 faces: a bounded map build)
    constexpr int WATER_EXCLUDER_HULL_MAX_VERTICES = 128;

    enum class WaterHullStatus{
        NONE,           // not a Convex Hull excluder
        PENDING,        // its model is still loading
        READY,
        NO_MESH,        // no mesh on the GameObject (nor mesh nodes of its model)
        FLAT,           // the mesh has no volume
        NO_PHYSICS      // a build without 3D physics: the mesh's bounds box stands in
    };

    // "none", "pending", "ready", "no_mesh", "flat", "no_physics"
    JASPERENGINE_API const char* getWaterHullStatusName(WaterHullStatus status);

    // A convex hull in its excluder's local space
    struct JASPERENGINE_API WaterHull{
        std::vector<Vector4> planes;        // xyz the outward normal, w its distance from the origin
        std::vector<Vector3> points;        // its vertices
        std::vector<std::pair<uint16_t, uint16_t>> edges;   // indices into points, each edge once
        AABB bounds;
        size_t sourceVertices = 0;          // the mesh vertices it was built from
    };

    // The hull of the points: they are snapped to a 1/1024 grid of their bounds and deduplicated
    // first (a dense mesh costs the hull builder only the cells it occupies). READY, FLAT (no
    // volume), NO_MESH (no points) or NO_PHYSICS (a build without Jolt: the hull is left empty and
    // the caller uses the points' bounds box).
    JASPERENGINE_API WaterHullStatus buildWaterHull(const std::vector<Vector3>& points, WaterHull& hull);

    // The local area a hull's map covers: its bounds with two texels around them in XZ
    JASPERENGINE_API void getWaterHullMapArea(const AABB& bounds, Vector3& origin, Vector3& extent);

    // The hull from above, WATER_EXCLUDER_HULL_MAP_SIZE square RGBA8 over getWaterHullMapArea:
    // R its bottom and G its top over each texel (0..1 of the area's height; an empty column
    // R 255, G 0), B 0.5 + how far the texel is outside its vertical walls / 8 texels (inside
    // under 0.5), A 255. Linear between texels, so a bilinear read keeps the edges straight.
    JASPERENGINE_API void buildWaterHullMap(const WaterHull& hull, std::vector<uint8_t>& rgba);

    // The local box of an excluder: its Box (Sphere: the box holding it), or its hull's bounds
    // (a NO_PHYSICS hull: the bounds of the mesh it stands for). Null when it keeps nothing out.
    JASPERENGINE_API AABB getWaterExcluderLocalBox(WaterExcluderShape shape, const Vector3& center, const Vector3& size,
                                                   const WaterHull* hull);

    // World to 0..1 across the volume the shader tests: the local box, or a hull's map area
    // (hull true). False for a null box or a model matrix without an inverse.
    JASPERENGINE_API bool getWaterExcluderUnitSpace(const AABB& localBox, bool hull, const Matrix4& modelMatrix, Matrix4& worldToUnit);

    // Whether a world point is inside: Box and Sphere through the unit space (a Sphere is the
    // ellipsoid inside its box), a hull by its exact planes in local space (empty planes: its
    // box, a NO_PHYSICS stand-in). False for a null box or a matrix without an inverse.
    JASPERENGINE_API bool isInsideWaterExcluder(WaterExcluderShape shape, const AABB& localBox, const WaterHull* hull,
                                                const Matrix4& modelMatrix, const Vector3& point);

}

#endif //WATER_EXCLUSION_H
