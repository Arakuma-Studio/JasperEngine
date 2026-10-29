// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Field names and defaults follow OpenEngine's SplineExtrude and SplineWall components
// (Engine/Include/Components/Spline/), Copyright (c) 2026 Gamecrafters Guild Limited and
// contributors, MIT License. See NOTICE.md.

#ifndef SPLINE_EXTRUDE_COMPONENT_H
#define SPLINE_EXTRUDE_COMPONENT_H

#include "math/Vector2.h"

#include <cstdint>
#include <vector>

namespace jasperengine{

    // the cross-section swept along the spline, or the area it fills (S18 R6, R7)
    enum class SplineExtrudeShape{
        Flat,           // a ribbon (2D: an edge in XY)
        Bevel,          // a path: width, edge drop and inset
        Crown,          // a road: crowned top, shoulders
        Rectangle,      // a wall: thickness x height, Racked or Stepped
        Tube,           // a pipe of the radius channel
        Custom,         // up to 32 profile points
        Fill,           // a closed spline's flat polygon (2D: in XY)
        WaterBanks      // a river or lake flooded over the terrain up to its banks
    };

    // where the half-width comes from
    enum class SplineWidthSource{
        Radius,         // each knot's radius, interpolated
        Constant        // width / 2 everywhere
    };

    enum class SplineCornerStyle{
        None,
        Mitre,          // sharp corners; past 120 degrees of turn built Round (and reported)
        Round
    };

    // how a wall or fence follows a slope
    enum class SplineGrade{
        Racked,         // tilts with the ground
        Stepped         // level runs with steps of stepHeight
    };

    // how generated geometry follows the terrain under it (S18 R9, D7)
    enum class SplineConform{
        None,
        TerrainHeight,
        TerrainHeightAndSlope
    };

    enum class SplineCollision{
        None,
        Mesh,           // static Jolt mesh shapes of the generated chunks (3D)
        Chain2D         // a Box2D chain of the centreline on the GameObject's Rigidbody2D (2D)
    };

    // one point of a Custom profile: x across (positive: right of travel), y up, in units of the
    // half-width for x and world units for y
    struct SplineProfilePoint{
        Vector2 position = Vector2(0.0f, 0.0f);
        bool hard = false;              // a crease: its two faces keep their own normals
        bool grounded = false;          // follows the ground (a wall's base)
    };

    // Sweeps a profile along the Spline of its GameObject into a mesh with world-metre UVs, or
    // fills it (Unity's SplineExtrude; S18 R6, R7). Its material, shadows and layer are the
    // GameObject's Mesh (MeshComponent), whose geometry SplineSystem generates in chunks.
    struct SplineExtrudeComponent{
        // component checkbox: a disabled extrude frees its chunks
        bool enabled = true;

        SplineExtrudeShape shape = SplineExtrudeShape::Bevel;
        SplineWidthSource widthSource = SplineWidthSource::Radius;
        float width = 2.0f;                 // full width with Constant (Rectangle: unused)

        // Bevel
        float edgeDrop = 0.1f;
        float edgeInset = 0.15f;
        // Crown
        float crownRise = 0.12f;
        float shoulderWidth = 0.5f;
        float shoulderDrop = 0.2f;
        // Rectangle
        float thickness = 0.6f;
        float height = 3.0f;
        SplineGrade grade = SplineGrade::Racked;
        float stepHeight = 0.5f;
        // Tube
        unsigned int tubeSides = 8;
        // Custom (at most 32 points)
        std::vector<SplineProfilePoint> customProfile;
        bool customClosed = false;
        // WaterBanks
        float maxHalfWidth = 12.0f;         // the flood stops this far from the centreline
        float waterEdgeDrop = 0.3f;         // world units the rim is buried under the banks
        bool seaLevelFloor = false;
        float seaLevel = 0.0f;              // world height the surface never drops below (seaLevelFloor)
        // flow encoding for library:water FLOW (vertex colour; Flat Channel rivers and WaterBanks)
        bool flow = false;
        float flowSpeed = 1.0f;             // 0-1 speed scale written to the vertex colour's blue

        // UVs: U = draped distance x tilesPerMetreU, V = profile perimeter x tilesPerMetreV
        float tilesPerMetreU = 1.0f;
        float tilesPerMetreV = 1.0f;
        bool levelCourseV = false;          // walls: V from world height, so courses stay level

        SplineCornerStyle corners = SplineCornerStyle::Mitre;
        bool endCaps = true;                // closed profiles of open splines
        float endTaper = 0.0f;              // world units the width tapers to 0 at both ends
        float lateralOffset = 0.0f;
        float verticalOffset = 0.0f;

        SplineConform conform = SplineConform::None;
        float slopeBlend = 1.0f;            // TerrainHeightAndSlope: 0 keeps up, 1 the terrain normal
        float stationSpacing = 1.0f;        // world units between stations at most

        SplineCollision collision = SplineCollision::None;
    };

}

#endif //SPLINE_EXTRUDE_COMPONENT_H
