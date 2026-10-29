// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineGeometry/Include/SplineGeometry/SplineStripBuilder.h,
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.
//
// Handedness (S18 D6): OpenEngine is left-handed with (right, up, forward) = (+X, +Y, +Z). Here
// right = normalize(forward x up), so (right, up, forward) is a left-handed triple of the cross
// product: positions and normals keep OpenEngine's meaning (a positive lateral coordinate is the
// right of travel), and every triangle is wound the other way, so that for each emitted triangle
// (a, b, c) the geometric normal (b - a) x (c - a) agrees with its vertex normals (front faces
// are counter-clockwise). Tangents carry U and their w makes cross(normal, tangent) * w point
// along increasing V (the MikkTSpace and glTF convention).

#ifndef SPLINE_SPLINESTRIPBUILDER_H
#define SPLINE_SPLINESTRIPBUILDER_H

#include "Export.h"
#include "math/AABB.h"
#include "SplineProfile.h"
#include "SplineStation.h"
#include "SplineVertex.h"
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace jasperengine{ namespace spline{

    // How the width channel scales the cross-section: a road that widens should not get taller,
    // a rampart (or a tube) that widens should.
    enum class SplineProfileScale : uint8_t{
        LateralOnly = 0,
        Uniform,
        None
    };

    // Chunk length of a chunked run, in station distance (S18 R30), and the vertex limit a
    // chunk respects so its indices fit the uint16 of a physics mesh collider (R14).
    inline constexpr float SPLINE_STRIP_CHUNK_LENGTH = 64.0f;
    inline constexpr uint32_t SPLINE_STRIP_MAX_CHUNK_VERTICES = 65535u;

    struct SplineStripParams{
        SplineProfileScale widthScale = SplineProfileScale::LateralOnly;
        // U = (distance - uOriginMetres) * tilesPerMetreU along the draped run; V = the scaled
        // profile perimeter * tilesPerMetreV. Never normalized: a 10 m and a 100 m wall get
        // different numbers of courses, not differently shaped ones.
        float tilesPerMetreU = 1.0f;
        float tilesPerMetreV = 1.0f;
        // Non-finite (the default) rebases U to the first station of the span built. A chunked run
        // passes one origin for every chunk, so the shared ring gets one U from both sides
        // (buildSplineStripChunk resolves it to the run's first station).
        float uOriginMetres = std::numeric_limits<float>::quiet_NaN();
        // Closed loop: nudge the U density so the span spans a whole number of tiles. It rounds
        // against the span of THIS call; buildSplineStripChunk snaps once over the whole run.
        bool snapUForClosedLoop = false;
        // Finite: a welded loop of this length whose faces each snap to a whole number of tiles. A
        // side face runs loopLengthMetres + |x| * the loop's face turn on its side (the last
        // station's faceTurnLeft/Right) and the top and bottom loopLengthMetres plus the loop's arc
        // allowance (the last station's topArcAllowance).
        float loopLengthMetres = std::numeric_limits<float>::quiet_NaN();
        float loopFaceTurnLeft = 0.0f;
        float loopFaceTurnRight = 0.0f;
        float loopTopArcAllowance = 0.0f;
        // metres of distance per unit of the profile's lateral offset, wherever U reads a face turn
        float lateralMetresPerUnit = 1.0f;
        // honoured for a closed profile with cap triangles only
        bool generateCaps = true;
        // Finite: side faces (edge normal more lateral than vertical), risers and caps take V as
        // the vertex's height above this datum (its y) times tilesPerMetreV, so coursed stone stays
        // level on a slope and meets itself across corners and chunks. Non-finite (the default)
        // keeps the perimeter V, which is what paths and roads want.
        float heightVDatumMetres = std::numeric_limits<float>::quiet_NaN();
        // metres along the station's (rolled) right, negative to the left, and along its up
        float lateralOffset = 0.0f;
        float verticalOffset = 0.0f;
        // A Channel river (S18 D9): vertex colour rg = the station's flow direction in XZ
        // (0.5 + 0.5 d), b = flowSpeedScale, a = 1. Off, the colour stays white.
        bool writeFlow = false;
        float flowSpeedScale = 1.0f;
    };

    struct SplineStripMesh{
        std::vector<SplineVertex> vertices;
        std::vector<uint32_t> indices;
        AABB bounds;
        // Vertices per station ring, including the extra slots creases and a closed profile's V
        // seam add. The first ringCount * ringVertexCount vertices are the rings in station order;
        // riser and cap vertices follow.
        uint32_t ringVertexCount = 0;
        uint32_t ringCount = 0;
        // stations dropped because they were not finite or did not advance past the previous one
        // (a corner's Crease and Pivot rings stand at their corner's distance by design and stay)
        uint32_t droppedStations = 0;

        bool isValid() const { return !vertices.empty() && !indices.empty(); }
    };

    // Sweep `profile` along `stations`, welding each ring to the next: consecutive rings share
    // their vertices, so the strip has no seam to misalign and no tile end to bury. Triangles
    // with two coincident corners (a round corner pivoting on its inner point, a ring pinched to
    // nothing) are not emitted. Empty when the profile has fewer than two points or fewer than two
    // stations survive the ascending-distance filter.
    JASPERENGINE_API SplineStripMesh buildSplineStrip(const SplineProfile& profile, std::span<const SplineStation> stations,
                                                      const SplineStripParams& params);
    // the same into `mesh`, reusing its storage
    JASPERENGINE_API void buildSplineStrip(const SplineProfile& profile, std::span<const SplineStation> stations,
                                           const SplineStripParams& params, SplineStripMesh& mesh);

    // One chunk of a chunked run: stations [firstStation, endStation) of the run's stream. The
    // last station of a chunk is the first of the next, so the two meet on one ring.
    struct SplineStripChunkRange{
        uint32_t firstStation = 0;
        uint32_t endStation = 0;
        float startDistance = 0.0f;
        float endDistance = 0.0f;
        // holds the run's first / last ring, and so its front / back cap
        bool opensRun = false;
        bool closesRun = false;
    };

    // Cut a run into chunks of about `chunkLength` (S18 R30). A boundary stands on the first
    // usable station at or past each multiple of chunkLength from the run's start that joins
    // plainly on both sides (an Advance ring, off any corner), so a local edit leaves the stations
    // and so the output of every other chunk unchanged; a chunk that would pass `maxVertices`
    // (rings, risers and caps counted exactly) ends early at its last plain station. Empty when
    // the run builds nothing.
    JASPERENGINE_API std::vector<SplineStripChunkRange> planSplineStripChunks(const SplineProfile& profile,
            std::span<const SplineStation> stations, const SplineStripParams& params,
            float chunkLength = SPLINE_STRIP_CHUNK_LENGTH, uint32_t maxVertices = SPLINE_STRIP_MAX_CHUNK_VERTICES);

    // Build one chunk of the run `stations` (the WHOLE run, which resolves the U axis: a
    // non-finite uOriginMetres becomes the run's first usable station and snapUForClosedLoop
    // rounds the density once over the run's whole length). Caps go on the run's ends only. The
    // result is the same, bit for bit, whichever chunks are rebuilt, and a ring shared by two
    // chunks is bit-identical in both.
    JASPERENGINE_API void buildSplineStripChunk(const SplineProfile& profile, std::span<const SplineStation> stations,
                                                const SplineStripChunkRange& chunk, const SplineStripParams& params,
                                                SplineStripMesh& mesh);

}}

#endif // SPLINE_SPLINESTRIPBUILDER_H
