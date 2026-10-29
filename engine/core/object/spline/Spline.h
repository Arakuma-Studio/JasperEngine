// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SPLINE_H
#define SPLINE_H

#include "object/EntityHandle.h"
#include "component/SplineComponent.h"
#include "spline/SplineTypes.h"

#include <vector>

namespace jasperengine{

    // A GameObject's Spline (SplineComponent) in world space (S18 R28): Lua
    // self.entity:getSpline() or Spline(scene, entity). Distances and radii are world units: the
    // curve's local values times the GameObject's largest axis scale (S18 D4). Knot indices are
    // 0-based, as in the MCP tools. Editing a knot regenerates what is built on the spline.
    class JASPERENGINE_API Spline: public EntityHandle{
    private:
        SplineComponent& component() const;
        Matrix4 localToWorld() const;
        float worldScale() const;
        spline::SplineFrame frameToWorld(const spline::SplineFrame& local) const;
        spline::SplineHit hitToWorld(const spline::SplineHit& local, const Vector3& query) const;

    public:
        Spline(Scene* scene, Entity entity);
        virtual ~Spline();

        Spline(const Spline& rhs);
        Spline& operator=(const Spline& rhs);
        Spline(Spline&& rhs) noexcept;
        Spline& operator=(Spline&& rhs) noexcept;

        // the entity has the component
        bool isValid() const;

        void setEnabled(bool enabled);
        bool isEnabled() const;
        void setType(spline::SplineType type);
        spline::SplineType getType() const;
        void setClosed(bool closed);
        bool isClosed() const;
        // false: up axis +Z, the XY plane (2D)
        void set3D(bool is3D);
        bool is3D() const;

        // knots (world positions, local handles as authored)
        size_t getKnotCount() const;
        void addKnot(Vector3 position);
        void insertKnot(size_t index, Vector3 position);
        bool removeKnot(size_t index);
        void clearKnots();
        Vector3 getKnotPosition(size_t index) const;
        bool setKnotPosition(size_t index, Vector3 position);
        float getKnotRadius(size_t index) const;
        bool setKnotRadius(size_t index, float radius);
        float getKnotRoll(size_t index) const;
        bool setKnotRoll(size_t index, float rollDeg);
        Vector3 getKnotTangentIn(size_t index) const;
        Vector3 getKnotTangentOut(size_t index) const;
        bool setKnotTangents(size_t index, Vector3 tangentIn, Vector3 tangentOut);
        spline::TangentMode getKnotMode(size_t index) const;
        bool setKnotMode(size_t index, spline::TangentMode mode);
        // every knot's world position
        std::vector<Vector3> getKnotPositions() const;
        // incremented by every change of the curve
        uint64_t getVersion() const;

        // queries (R3), world space
        float getLength() const;
        spline::SplineFrame evaluate(float t) const;
        spline::SplineFrame evaluateAtDistance(float distance) const;
        Vector3 getPosition(float t) const;
        float tToDistance(float t) const;
        float distanceToT(float distance) const;
        spline::SplineHit getClosestPoint(Vector3 point) const;
        spline::SplineHit getClosestPointXZ(float x, float z) const;
        float getSignedDistance(Vector3 point) const;
        float getSignedDistanceXZ(float x, float z) const;
        float getSignedDistanceBandXZ(float x, float z) const;
        std::vector<spline::SplineFrame> sampleUniform(unsigned int count) const;
        // knots resampled every spacing world units, channels kept
        bool resample(float spacing);
        // Douglas-Peucker on the knots: removes those within tolerance world units of the curve
        // through the others; returns how many it removed
        size_t simplify(float tolerance);
    };

}

#endif //SPLINE_H
