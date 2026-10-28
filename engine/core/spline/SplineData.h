// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/Spline/Include/Spline/SplineData.h, Copyright (c) 2026
// Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#ifndef SPLINE_SPLINEDATA_H
#define SPLINE_SPLINEDATA_H

#include "Export.h"
#include "SplineTypes.h"
#include "math/Vector3.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

namespace jasperengine{ namespace spline{

    // The axis-aligned box of one segment's curve (the radius is not included).
    struct SplineSegmentBounds{
        Vector3 min;
        Vector3 max;
    };

    // What the queries read, derived from the knots and rebuilt lazily whenever the spline's
    // version changes (S18 R5). Its size stays within 4 KB + 64 B per knot (R22): the arc
    // resolution per segment (arcSteps) is the largest that fits, between 1 and 32.
    struct SplineCache{
        uint32_t segmentCount = 0;
        // arc-length intervals per segment, uniform in the segment parameter u
        uint32_t arcSteps = 0;
        float length = 0.0f;
        // closed splines: the largest distance (with margin) between the XZ polygon and the curve,
        // and the polygon's signed area in (x, z) (its sign is the loop's orientation)
        float polygonError = 0.0f;
        float polygonArea = 0.0f;
        // per segment the two inner Bezier control points (b1, b2), absolute: every type runs as
        // one cubic Bezier per segment from knot i to knot i + 1 (Linear at thirds of the chord,
        // CatmullRom from its centripetal tangents, CubicBezier from the handles)
        std::vector<Vector3> controls;
        std::vector<SplineSegmentBounds> bounds;
        // arc[segment * arcSteps + i] = arc length from the start to u = (i + 1) / arcSteps of
        // that segment; ascending, its last entry is the length
        std::vector<float> arc;
        // closed splines: the curve at every u = i / arcSteps, as interleaved x, z
        std::vector<float> polygonXZ;
    };

    // Which handle of a Bezier knot was edited.
    enum class SplineHandle : uint8_t{
        In = 0,
        Out
    };

    // Makes the other handle follow the edited one under the knot's tangent mode (Unity):
    // Mirrored copies it negated, Aligned turns the other one opposite while keeping its own
    // length, Broken leaves it. A zero edited handle has no direction: Aligned keeps the other.
    JASPERENGINE_API void applyTangentMode(SplineKnot& knot, SplineHandle edited);

    // A spline: its type, its closed flag, its up axis and its knots, plus the lazily built
    // cache its queries read (SplineEvaluator.h). Every mutation goes through a setter that bumps
    // version() (or an editable accessor followed by markDirty()), so a stale cache can never be
    // read (R5).
    //
    // Thread safety: const queries may run on several threads at once, also on a spline whose
    // cache is stale (the rebuild is guarded by a mutex and published through an atomic version;
    // calling ensureCache() once before fanning out avoids the threads waiting on that rebuild).
    // A mutation must not run while another thread queries the same spline.
    class JASPERENGINE_API SplineData{
    public:
        // fewest knots that describe a curve
        static constexpr size_t MIN_KNOTS = 2;
        // fewest knots a ring needs to enclose anything (two would retrace A -> B -> A)
        static constexpr size_t MIN_CLOSED_KNOTS = 3;

        SplineData() = default;
        // copies the definition and its version; the copy builds its own cache when first queried
        SplineData(const SplineData& other);
        SplineData& operator=(const SplineData& other);

        SplineType type() const { return splineType; }
        void setType(SplineType type);

        // the authored flag (serialization, the inspector); geometry asks effectivelyClosed()
        bool closed() const { return isClosed; }
        void setClosed(bool closed);
        // closed with at least MIN_CLOSED_KNOTS: a closed spline of two knots generates as one
        // open segment while closed() stays as authored, so a third knot restores the ring
        bool effectivelyClosed() const { return isClosed && splineKnots.size() >= MIN_CLOSED_KNOTS; }

        // +Y in 3D, +Z for a 2D spline in the XY plane; normalized (a zero axis is refused)
        const Vector3& upAxis() const { return splineUpAxis; }
        void setUpAxis(const Vector3& axis);

        const std::vector<SplineKnot>& knots() const { return splineKnots; }
        size_t knotCount() const { return splineKnots.size(); }
        const SplineKnot& knot(size_t index) const { return splineKnots[index]; }

        // Knot edits. Radii are clamped to >= 0 (NaN to 0). An index out of range is refused
        // (false, nothing changes, the version stays).
        void setKnots(std::vector<SplineKnot> knots);
        bool setKnot(size_t index, const SplineKnot& knot);
        bool setKnotPosition(size_t index, const Vector3& position);
        bool setKnotRadius(size_t index, float radius);
        bool setKnotRoll(size_t index, float rollDeg);
        // both handles as given, whatever the mode
        bool setKnotTangents(size_t index, const Vector3& tangentIn, const Vector3& tangentOut);
        // one handle; the other follows the knot's mode (applyTangentMode)
        bool setKnotTangentIn(size_t index, const Vector3& tangentIn);
        bool setKnotTangentOut(size_t index, const Vector3& tangentOut);
        // the new mode applies at once, with tangentOut leading
        bool setKnotMode(size_t index, TangentMode mode);
        void addKnot(const SplineKnot& knot);
        // index past the end appends
        void insertKnot(size_t index, const SplineKnot& knot);
        // refused (false) out of range or when only MIN_KNOTS remain
        bool removeKnot(size_t index);
        // One knot per frame, carrying the channels (radius, roll): the resample primitive. For
        // CubicBezier the handles are fabricated from centred differences of the new positions
        // (Catmull-Rom-like smoothness, Mirrored; wrapping on a closed ring).
        void replaceKnotsFromFrames(std::span<const SplineFrame> frames);

        // In-place access for the editor's property writes (Catalog), which write through raw
        // pointers. These bypass the setters: the caller MUST call markDirty() after writing, or
        // the queries keep answering for the previous definition. Radii written here are not
        // clamped; the up axis must stay unit length.
        std::vector<SplineKnot>& editableKnots() { return splineKnots; }
        SplineType& editableType() { return splineType; }
        bool& editableClosed() { return isClosed; }
        Vector3& editableUpAxis() { return splineUpAxis; }

        // bumps the version (forces the cache and the consumers to rebuild); also what an
        // in-place write through the editable accessors must be followed by
        void markDirty() { ++dataVersion; }
        // increments on every change; starts at 1
        uint64_t version() const { return dataVersion; }

        bool isValid() const { return splineKnots.size() >= MIN_KNOTS; }
        // effectively closed: one per knot; open: knots - 1; 0 below MIN_KNOTS
        uint32_t segmentCount() const;

        // the cache of the current version, rebuilt first if stale
        const SplineCache& ensureCache() const;
        // heap and object bytes of the cache (after ensureCache): <= 4096 + 64 * knots (R22)
        size_t cacheBytes() const;

    private:
        void rebuildCache(SplineCache& cache) const;

        SplineType splineType = SplineType::CatmullRom;
        bool isClosed = false;
        Vector3 splineUpAxis = Vector3(0.0f, 1.0f, 0.0f);
        std::vector<SplineKnot> splineKnots;
        uint64_t dataVersion = 1;

        mutable SplineCache cache;
        // the version cache was built for (0: never)
        mutable std::atomic<uint64_t> cacheVersion{0};
        mutable std::mutex cacheMutex;
    };

}}

#endif // SPLINE_SPLINEDATA_H
