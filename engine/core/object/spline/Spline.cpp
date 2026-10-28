// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Spline.h"

#include "Scene.h"
#include "component/Transform.h"
#include "spline/SplineEvaluator.h"
#include "spline/SplineUtility.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

Spline::Spline(Scene* scene, Entity entity): EntityHandle(scene, entity){
}

Spline::~Spline() = default;
Spline::Spline(const Spline& rhs) = default;
Spline& Spline::operator=(const Spline& rhs) = default;
Spline::Spline(Spline&& rhs) noexcept = default;
Spline& Spline::operator=(Spline&& rhs) noexcept = default;

SplineComponent& Spline::component() const{
    return getComponent<SplineComponent>();
}

bool Spline::isValid() const{
    return scene && scene->findComponent<SplineComponent>(entity) != nullptr;
}

Matrix4 Spline::localToWorld() const{
    const Transform* transform = scene->findComponent<Transform>(entity);
    return transform ? transform->modelMatrix : Matrix4();
}

float Spline::worldScale() const{
    const Transform* transform = scene->findComponent<Transform>(entity);
    if (!transform){
        return 1.0f;
    }
    const Vector3& s = transform->worldScale;
    const float scale = std::max(std::fabs(s.x), std::max(std::fabs(s.y), std::fabs(s.z)));
    return scale > 0.0f ? scale : 1.0f;
}

spline::SplineFrame Spline::frameToWorld(const spline::SplineFrame& local) const{
    const Matrix4 m = localToWorld();
    const float scale = worldScale();
    spline::SplineFrame world = local;
    world.position = m * local.position;
    // directions through the linear part only (no translation), then unit length again
    const Vector3 origin = m * Vector3(0.0f, 0.0f, 0.0f);
    world.forward = (m * local.forward - origin).normalized();
    world.right = (m * local.right - origin).normalized();
    world.up = (m * local.up - origin).normalized();
    world.radius = local.radius * scale;
    return world;
}

spline::SplineHit Spline::hitToWorld(const spline::SplineHit& local, const Vector3& query) const{
    spline::SplineHit world = local;
    world.position = localToWorld() * local.position;
    world.distance = (world.position - query).length();
    return world;
}

void Spline::setEnabled(bool enabled){ component().enabled = enabled; }
bool Spline::isEnabled() const{ return component().enabled; }

void Spline::setType(spline::SplineType type){ component().spline.setType(type); }
spline::SplineType Spline::getType() const{ return component().spline.type(); }

void Spline::setClosed(bool closed){ component().spline.setClosed(closed); }
bool Spline::isClosed() const{ return component().spline.closed(); }

void Spline::set3D(bool is3D){ component().spline.setUpAxis(is3D ? Vector3(0.0f, 1.0f, 0.0f) : Vector3(0.0f, 0.0f, 1.0f)); }
bool Spline::is3D() const{ return component().spline.upAxis().z == 0.0f; }

size_t Spline::getKnotCount() const{ return component().spline.knotCount(); }

void Spline::addKnot(Vector3 position){
    SplineComponent& spline = component();
    spline::SplineKnot knot;
    if (spline.spline.knotCount() > 0){
        knot = spline.spline.knot(spline.spline.knotCount() - 1);
        knot.tangentIn = Vector3(0.0f, 0.0f, 0.0f);
        knot.tangentOut = Vector3(0.0f, 0.0f, 0.0f);
    }
    knot.position = localToWorld().inverse() * position;
    spline.spline.addKnot(knot);
}

void Spline::insertKnot(size_t index, Vector3 position){
    SplineComponent& spline = component();
    spline::SplineKnot knot;
    if (spline.spline.knotCount() > 0){
        knot = spline.spline.knot(std::min(index, spline.spline.knotCount() - 1));
        knot.tangentIn = Vector3(0.0f, 0.0f, 0.0f);
        knot.tangentOut = Vector3(0.0f, 0.0f, 0.0f);
    }
    knot.position = localToWorld().inverse() * position;
    spline.spline.insertKnot(index, knot);
}

bool Spline::removeKnot(size_t index){ return component().spline.removeKnot(index); }
void Spline::clearKnots(){ component().spline.setKnots({}); }

Vector3 Spline::getKnotPosition(size_t index) const{
    const SplineComponent& spline = component();
    if (index >= spline.spline.knotCount()){
        return Vector3(0.0f, 0.0f, 0.0f);
    }
    return localToWorld() * spline.spline.knot(index).position;
}

bool Spline::setKnotPosition(size_t index, Vector3 position){
    return component().spline.setKnotPosition(index, localToWorld().inverse() * position);
}

float Spline::getKnotRadius(size_t index) const{
    const SplineComponent& spline = component();
    return index < spline.spline.knotCount() ? spline.spline.knot(index).radius * worldScale() : 0.0f;
}

bool Spline::setKnotRadius(size_t index, float radius){
    return component().spline.setKnotRadius(index, radius / worldScale());
}

float Spline::getKnotRoll(size_t index) const{
    const SplineComponent& spline = component();
    return index < spline.spline.knotCount() ? spline.spline.knot(index).rollDeg : 0.0f;
}

bool Spline::setKnotRoll(size_t index, float rollDeg){ return component().spline.setKnotRoll(index, rollDeg); }

Vector3 Spline::getKnotTangentIn(size_t index) const{
    const SplineComponent& spline = component();
    return index < spline.spline.knotCount() ? spline.spline.knot(index).tangentIn : Vector3(0.0f, 0.0f, 0.0f);
}

Vector3 Spline::getKnotTangentOut(size_t index) const{
    const SplineComponent& spline = component();
    return index < spline.spline.knotCount() ? spline.spline.knot(index).tangentOut : Vector3(0.0f, 0.0f, 0.0f);
}

bool Spline::setKnotTangents(size_t index, Vector3 tangentIn, Vector3 tangentOut){
    return component().spline.setKnotTangents(index, tangentIn, tangentOut);
}

spline::TangentMode Spline::getKnotMode(size_t index) const{
    const SplineComponent& spline = component();
    return index < spline.spline.knotCount() ? spline.spline.knot(index).mode : spline::TangentMode::Mirrored;
}

bool Spline::setKnotMode(size_t index, spline::TangentMode mode){ return component().spline.setKnotMode(index, mode); }

std::vector<Vector3> Spline::getKnotPositions() const{
    const SplineComponent& spline = component();
    const Matrix4 m = localToWorld();
    std::vector<Vector3> positions;
    positions.reserve(spline.spline.knotCount());
    for (const spline::SplineKnot& knot : spline.spline.knots()){
        positions.push_back(m * knot.position);
    }
    return positions;
}

uint64_t Spline::getVersion() const{ return component().spline.version(); }

float Spline::getLength() const{
    return spline::length(component().spline) * worldScale();
}

spline::SplineFrame Spline::evaluate(float t) const{
    return frameToWorld(spline::evaluate(component().spline, t));
}

spline::SplineFrame Spline::evaluateAtDistance(float distance) const{
    return frameToWorld(spline::evaluateAtDistance(component().spline, distance / worldScale()));
}

Vector3 Spline::getPosition(float t) const{
    return localToWorld() * spline::evaluatePosition(component().spline, t);
}

float Spline::tToDistance(float t) const{
    return spline::tToDistance(component().spline, t) * worldScale();
}

float Spline::distanceToT(float distance) const{
    return spline::distanceToT(component().spline, distance / worldScale());
}

spline::SplineHit Spline::getClosestPoint(Vector3 point) const{
    const Vector3 local = localToWorld().inverse() * point;
    return hitToWorld(spline::closestPoint(component().spline, local), point);
}

spline::SplineHit Spline::getClosestPointXZ(float x, float z) const{
    const Matrix4 m = localToWorld();
    const Vector3 local = m.inverse() * Vector3(x, (m * Vector3(0.0f, 0.0f, 0.0f)).y, z);
    spline::SplineHit hit = spline::closestPointXZ(component().spline, local.x, local.z);
    hit.position = m * hit.position;
    const float dx = hit.position.x - x;
    const float dz = hit.position.z - z;
    hit.distance = std::sqrt(dx * dx + dz * dz);
    return hit;
}

float Spline::getSignedDistance(Vector3 point) const{
    const Vector3 local = localToWorld().inverse() * point;
    return spline::signedDistance(component().spline, local) * worldScale();
}

float Spline::getSignedDistanceXZ(float x, float z) const{
    const Matrix4 m = localToWorld();
    const Vector3 local = m.inverse() * Vector3(x, (m * Vector3(0.0f, 0.0f, 0.0f)).y, z);
    return spline::signedDistanceXZ(component().spline, local.x, local.z) * worldScale();
}

float Spline::getSignedDistanceBandXZ(float x, float z) const{
    const Matrix4 m = localToWorld();
    const Vector3 local = m.inverse() * Vector3(x, (m * Vector3(0.0f, 0.0f, 0.0f)).y, z);
    return spline::signedDistanceBandXZ(component().spline, local.x, local.z) * worldScale();
}

std::vector<spline::SplineFrame> Spline::sampleUniform(unsigned int count) const{
    std::vector<spline::SplineFrame> frames;
    spline::sampleUniform(component().spline, count, frames);
    for (spline::SplineFrame& frame : frames){
        frame = frameToWorld(frame);
    }
    return frames;
}

bool Spline::resample(float spacing){
    return spline::resample(component().spline, spacing / worldScale());
}

size_t Spline::simplify(float tolerance){
    SplineComponent& spline = component();
    const std::vector<spline::SplineKnot>& knots = spline.spline.knots();
    if (knots.size() <= spline::SplineData::MIN_KNOTS){
        return 0;
    }
    std::vector<Vector3> positions;
    positions.reserve(knots.size());
    for (const spline::SplineKnot& knot : knots){
        positions.push_back(knot.position);
    }
    std::vector<uint32_t> kept;
    spline::simplifyIndices(positions, tolerance / worldScale(), kept);
    if (kept.size() >= knots.size() || kept.size() < spline::SplineData::MIN_KNOTS){
        return 0;
    }
    std::vector<spline::SplineKnot> simplified;
    simplified.reserve(kept.size());
    for (uint32_t index : kept){
        simplified.push_back(knots[index]);
    }
    const size_t removed = knots.size() - simplified.size();
    spline.spline.setKnots(std::move(simplified));
    return removed;
}
