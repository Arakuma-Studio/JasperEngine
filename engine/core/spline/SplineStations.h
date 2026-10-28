// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/TerrainECS/{Include/TerrainECS,Source}/
// TerrainRoutePolyline.* (the resampled route and its segment-nearest query) and the station
// frames of Apps/Editor/Source/Placement/SplineSweepStations.cpp, Copyright (c) 2026 Gamecrafters
// Guild Limited and contributors, MIT License. See NOTICE.md.

#ifndef SPLINE_SPLINESTATIONS_H
#define SPLINE_SPLINESTATIONS_H

// Station sampling (S18 D5): the one draped polyline that the terrain bake, the extrude, the
// placement and the fill of a spline version all consume.

#include "Export.h"
#include "SplineData.h"
#include "SplineStation.h"
#include "SplineTypes.h"
#include "math/Matrix4.h"
#include "math/Vector3.h"
#include <cstdint>
#include <span>
#include <vector>

namespace jasperengine{ namespace spline{

    // the shortest station spacing a resample honours (OpenEngine kMinRouteStationSpacing)
    inline constexpr float SPLINE_MIN_STATION_SPACING = 0.05f;
    // the most stations one build or resample emits; past it the spacing widens
    inline constexpr uint32_t SPLINE_MAX_STATIONS = 1u << 20;

    // How stations follow the ground under them (S18 R9).
    enum class SplineConform : uint8_t{
        // the curve as authored
        None = 0,
        // the probed height (plus verticalOffset) replaces the curve's
        Height,
        // Height, and up tilts toward the surface normal by slopeBlend
        HeightAndSlope
    };

    struct SplineStationOptions{
        // the largest distance between stations along the curve (local units), > 0
        float stationSpacing = 1.0f;
        // an interval is halved where its ends' tangents turn by more than this (degrees)
        float maxTurnDeg = 4.0f;
        // ... or where the conformed height at its middle strays from the chord by more than this
        float maxHeightDelta = 0.1f;
        // how many times one interval may be halved
        uint32_t maxSubdivisions = 5;

        SplineConform conform = SplineConform::None;
        // probes the surface under a world point (height along upAxis); unused with None
        SplineSurfaceProbe probe;
        // HeightAndSlope: 0 keeps the curve's up, 1 takes the surface normal
        float slopeBlend = 1.0f;
        // added to every probed (or held) height
        float verticalOffset = 0.0f;

        // a constant half-width on both sides instead of the radius channel
        bool constantWidth = false;
        float halfWidth = 1.0f;

        // Local -> world (affine) applied to the curve before it is probed. Station positions,
        // frames and distances are then in world space, with the radius channel scaled by the
        // largest axis scale (S18 D4); upAxis is taken as the world up. Null keeps local space.
        const Matrix4* localToWorld = nullptr;
    };

    // What buildStations met.
    struct SplineStationReport{
        // probes that found no surface (held across, R9)
        uint32_t probeMisses = 0;
        // every probe missed: the stations keep the authored heights
        bool allMissed = false;
        // the station budget widened the spacing
        bool capped = false;
    };

    // Adaptive stations of spline (D5): every knot is a station (splineT = its index), and
    // between knots they are at most stationSpacing apart along the curve, with extra stations
    // where the tangent turns by more than maxTurnDeg or (conforming) the drape strays by more
    // than maxHeightDelta. Conforming, a probe that misses holds the nearest measured height: the
    // run's before it, or for a leading gap the first measured one after it (OpenEngine
    // HoldSurfaceAcrossGaps). distance is the DRAPED 3D length, strictly ascending (coincident
    // points are dropped); splineT is segment + u (knot index + fraction); rollRadians and the
    // half-widths come from the channels. The frames are roll-free and orthonormal per R4, from
    // the draped polyline itself (forward along the centred difference of the neighbours, right
    // = normalize(forward x up), parallel-transported where forward is parallel to up), so
    // consecutive stations never lean apart. A closed spline's last station repeats its first,
    // with the same frame. out is replaced; fewer than two knots leave it empty.
    JASPERENGINE_API SplineStationReport buildStations(const SplineData& spline, const SplineStationOptions& options,
                                                       std::vector<SplineStation>& out);

    // Splits every interval of stations longer than maxSpacing (at least
    // SPLINE_MIN_STATION_SPACING) into equal parts, interpolating the inserted stations linearly
    // (splineT -1, join Advance): the terrain bake's half-texel rule (S18 D8). out is replaced.
    JASPERENGINE_API void resampleStations(std::span<const SplineStation> stations, float maxSpacing,
                                           std::vector<SplineStation>& out);

    // Where a point lies relative to the station polyline in XZ.
    struct SplineStationHitXZ{
        // false for fewer than two stations (or nothing within the search radius)
        bool valid = false;
        // the segment stations[segment] -> stations[segment + 1] and the fraction along it
        uint32_t segment = 0;
        float fraction = 0.0f;
        // XZ distance to the polyline (+infinity when invalid)
        float distanceXZ = 0.0f;
        // signed XZ offset from the segment, positive on the right of travel
        float lateral = 0.0f;
        // interpolated along the segment: the station position's y, distance and half-widths
        float height = 0.0f;
        float distance = 0.0f;
        float halfWidthLeft = 0.0f;
        float halfWidthRight = 0.0f;
    };

    // Segment-nearest in XZ: the projection clamped to every segment, the first nearest segment
    // winning a tie; segments without XZ extent are skipped. O(stations).
    JASPERENGINE_API SplineStationHitXZ closestStationSegmentXZ(std::span<const SplineStation> stations, float x, float z);

    // closestStationSegmentXZ through a uniform XZ grid of the segments, for many queries (a
    // bake per texel). The stations are referenced, not copied: keep them alive and unchanged
    // while the grid is used. Results equal the linear query's.
    class JASPERENGINE_API SplineStationGridXZ{
    public:
        // cellSize <= 0 picks one from the segment lengths
        void build(std::span<const SplineStation> stations, float cellSize = 0.0f);
        // nothing farther than maxDistance is reported (valid false)
        SplineStationHitXZ closest(float x, float z, float maxDistance = 3.402823466e38f) const;

    private:
        std::span<const SplineStation> stations;
        float minX = 0.0f;
        float minZ = 0.0f;
        float cell = 1.0f;
        int32_t columns = 0;
        int32_t rows = 0;
        // CSR: the segments of cell c are items[cellStart[c] .. cellStart[c + 1])
        std::vector<uint32_t> cellStart;
        std::vector<uint32_t> items;
    };

}}

#endif // SPLINE_SPLINESTATIONS_H
