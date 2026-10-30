// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LatticeSystem.h"

#include "Scene.h"
#include "subsystem/RenderSystem.h"
#include "component/Transform.h"
#include "component/LatticeFollowerComponent.h"
#include "lattice/LatticeTypes.h"
#include "lattice/LatticeSolver.h"
#include "lattice/LatticePresets.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace {

    // the jiggle sleeps once every point's speed and offset are under this (spec §6.2)
    constexpr float JIGGLE_SLEEP = 1e-4f;
    // longest integration substep
    constexpr double JIGGLE_MAX_STEP = 1.0 / 120.0;
    // substeps per frame at most (a very stiff spring is integrated with smaller steps)
    constexpr int JIGGLE_MAX_SUBSTEPS = 64;
    // a frame longer than this (a hitch, a breakpoint) restarts the acceleration history
    constexpr double JIGGLE_MAX_FRAME = 0.25;
    // a jump of more than this many lattice sizes in one frame is a teleport
    constexpr float JIGGLE_TELEPORT_SIZES = 2.0f;

    constexpr float EPSILON = 1e-8f;

    int clampResolution(int value){
        return std::clamp(value, LATTICE_MIN_RESOLUTION, LATTICE_MAX_RESOLUTION);
    }

    Vector3 translationOf(const Matrix4& matrix){
        return Vector3(matrix[3][0], matrix[3][1], matrix[3][2]);
    }

    // largest axis length of a linear part (the lattice's world size along its longest axis)
    float maxAxisLength(const Matrix3& linear){
        return std::max(linear.column(0).length(), std::max(linear.column(1).length(), linear.column(2).length()));
    }

    bool isFinite(const Quaternion& q){
        return std::isfinite(q.w) && std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z);
    }

    bool isInvertible(const Matrix3& linear){
        return linear.isValid() && std::fabs(linear.determinant()) > EPSILON;
    }

    // 0 at the anchored face, 1 at the opposite one (spec §6.2)
    float anchorWeight(LatticeJiggleAnchor anchor, const Vector3& rest){
        switch (anchor){
            case LatticeJiggleAnchor::NEGATIVE_X: return rest.x + 0.5f;
            case LatticeJiggleAnchor::POSITIVE_X: return 0.5f - rest.x;
            case LatticeJiggleAnchor::NEGATIVE_Y: return rest.y + 0.5f;
            case LatticeJiggleAnchor::POSITIVE_Y: return 0.5f - rest.y;
            case LatticeJiggleAnchor::NEGATIVE_Z: return rest.z + 0.5f;
            case LatticeJiggleAnchor::POSITIVE_Z: return 0.5f - rest.z;
            case LatticeJiggleAnchor::NONE: break;
        }
        return 1.0f;
    }

    // the resolution an offsets list of `size` points was made at: of the factorizations
    // into three resolutions, the closest to the lattice's (x before y before z on a tie)
    bool inferResolution(size_t size, int rx, int ry, int rz, int& px, int& py, int& pz){
        int best = -1;
        for (int z = LATTICE_MIN_RESOLUTION; z <= LATTICE_MAX_RESOLUTION; z++){
            for (int y = LATTICE_MIN_RESOLUTION; y <= LATTICE_MAX_RESOLUTION; y++){
                for (int x = LATTICE_MIN_RESOLUTION; x <= LATTICE_MAX_RESOLUTION; x++){
                    if ((size_t)(x * y * z) != size){
                        continue;
                    }
                    const int distance = std::abs(x - rx) + std::abs(y - ry) + std::abs(z - rz);
                    if (best < 0 || distance < best){
                        best = distance;
                        px = x;
                        py = y;
                        pz = z;
                    }
                }
            }
        }
        return best >= 0;
    }

    // offsets sized for (rx, ry, rz): kept, resampled from the resolution their size gives,
    // or zero when no resolution has that size
    std::vector<Vector3> fitOffsets(const std::vector<Vector3>& offsets, int rx, int ry, int rz){
        const size_t count = (size_t)(rx * ry * rz);
        if (offsets.size() == count){
            return offsets;
        }
        int px = 0, py = 0, pz = 0;
        if (!offsets.empty() && inferResolution(offsets.size(), rx, ry, rz, px, py, pz)){
            return lattice::resample(px, py, pz, offsets, rx, ry, rz);
        }
        return std::vector<Vector3>(count, Vector3(0, 0, 0));
    }

    float easeValue(EaseType ease, float time){
        // a custom ease is a function the component cannot hold, a curve ease a curve: linear
        if (ease == EaseType::CUSTOM || ease == EaseType::CURVE){
            return time;
        }
        return Ease::getFunction(ease)(time);
    }

}

LatticeSystem::LatticeSystem(Scene* scene): SubSystem(scene){
    signature.set(scene->getComponentId<LatticeComponent>());
}

LatticeSystem::~LatticeSystem(){
}

void LatticeSystem::getResolution(const LatticeComponent& lattice, int& rx, int& ry, int& rz){
    rx = clampResolution(lattice.resolutionX);
    ry = clampResolution(lattice.resolutionY);
    rz = clampResolution(lattice.resolutionZ);
}

void LatticeSystem::setResolution(LatticeComponent& lattice, int rx, int ry, int rz){
    rx = clampResolution(rx);
    ry = clampResolution(ry);
    rz = clampResolution(rz);

    int ox, oy, oz;
    getResolution(lattice, ox, oy, oz);

    // what was saved at the old resolution goes with it; a pose of another size is
    // resampled from the resolution its size gives (getPoseOffsets)
    const size_t oldCount = (size_t)(ox * oy * oz);
    auto resampleList = [&](std::vector<Vector3>& offsets){
        if (offsets.size() == oldCount){
            offsets = lattice::resample(ox, oy, oz, offsets, rx, ry, rz);
        }else{
            offsets = fitOffsets(offsets, rx, ry, rz);
        }
    };

    resampleList(lattice.offsets);
    for (LatticePose& pose : lattice.poses){
        resampleList(pose.offsets);
    }
    if (lattice.tweening){
        resampleList(lattice.tweenFrom);
        resampleList(lattice.tweenTo);
    }

    lattice.resolutionX = rx;
    lattice.resolutionY = ry;
    lattice.resolutionZ = rz;

    resetJiggle(lattice);
    lattice.revision++;
}

const LatticePose* LatticeSystem::findPose(const LatticeComponent& lattice, const std::string& name){
    for (const LatticePose& pose : lattice.poses){
        if (pose.name == name){
            return &pose;
        }
    }
    return nullptr;
}

std::vector<Vector3> LatticeSystem::getPoseOffsets(const LatticeComponent& lattice, const LatticePose& pose){
    int rx, ry, rz;
    getResolution(lattice, rx, ry, rz);
    return fitOffsets(pose.offsets, rx, ry, rz);
}

void LatticeSystem::savePose(LatticeComponent& lattice, const std::string& name){
    int rx, ry, rz;
    getResolution(lattice, rx, ry, rz);
    std::vector<Vector3> offsets = fitOffsets(lattice.offsets, rx, ry, rz);

    for (LatticePose& pose : lattice.poses){
        if (pose.name == name){
            pose.offsets = std::move(offsets);
            lattice.revision++;
            return;
        }
    }
    LatticePose pose;
    pose.name = name;
    pose.offsets = std::move(offsets);
    lattice.poses.push_back(std::move(pose));
    lattice.revision++;
}

bool LatticeSystem::applyPose(LatticeComponent& lattice, const std::string& name, float weight){
    const LatticePose* pose = findPose(lattice, name);
    if (!pose){
        return false;
    }
    std::vector<Vector3> offsets = getPoseOffsets(lattice, *pose);
    for (Vector3& offset : offsets){
        offset = offset * weight;
    }
    stopTween(lattice);
    lattice.offsets = std::move(offsets);
    lattice.revision++;
    return true;
}

void LatticeSystem::startTween(LatticeComponent& lattice, const std::vector<Vector3>& target, float duration, EaseType ease){
    int rx, ry, rz;
    getResolution(lattice, rx, ry, rz);
    std::vector<Vector3> to = fitOffsets(target, rx, ry, rz);

    if (!(duration > 0.0f)){
        stopTween(lattice);
        lattice.offsets = std::move(to);
        lattice.revision++;
        return;
    }

    lattice.tweenFrom = fitOffsets(lattice.offsets, rx, ry, rz);
    lattice.tweenTo = std::move(to);
    lattice.tweenTime = 0.0f;
    lattice.tweenDuration = duration;
    lattice.tweenEase = ease;
    lattice.tweening = true;
}

void LatticeSystem::stopTween(LatticeComponent& lattice){
    lattice.tweening = false;
    lattice.tweenTime = 0.0f;
    lattice.tweenDuration = 0.0f;
    lattice.tweenFrom.clear();
    lattice.tweenTo.clear();
}

void LatticeSystem::resetJiggle(LatticeComponent& lattice){
    if (!lattice.dynamicOffsets.empty()){
        lattice.revision++;
    }
    lattice.dynamicOffsets.clear();
    lattice.jiggleVelocities.clear();
    lattice.jiggleLastPosition = Vector3(0, 0, 0);
    lattice.jiggleLastVelocity = Vector3(0, 0, 0);
    lattice.jiggleWarmup = 0;
    lattice.jiggleSleeping = true;
}

namespace {

    // jiggle arrays sized for the lattice, awake
    void wakeJiggle(LatticeComponent& lattice){
        int rx, ry, rz;
        LatticeSystem::getResolution(lattice, rx, ry, rz);
        const size_t count = (size_t)(rx * ry * rz);
        if (lattice.dynamicOffsets.size() != count || lattice.jiggleVelocities.size() != count){
            lattice.dynamicOffsets.assign(count, Vector3(0, 0, 0));
            lattice.jiggleVelocities.assign(count, Vector3(0, 0, 0));
        }
        lattice.jiggleSleeping = false;
    }

}

void LatticeSystem::poke(LatticeComponent& lattice, int index, const Vector3& localImpulse){
    int rx, ry, rz;
    getResolution(lattice, rx, ry, rz);
    if (!lattice.jiggle || index < 0 || index >= rx * ry * rz){
        return;
    }
    wakeJiggle(lattice);
    lattice.jiggleVelocities[index] += localImpulse;
}

void LatticeSystem::pokeAt(LatticeComponent& lattice, const Matrix4& latticeModel, const Vector3& worldPoint,
                           const Vector3& worldImpulse, float worldRadius){
    const Matrix3 linear = latticeModel.linear();
    if (!lattice.jiggle || !isInvertible(linear)){
        return;
    }
    const Vector3 localImpulse = linear.inverse(EPSILON) * worldImpulse;

    int rx, ry, rz;
    getResolution(lattice, rx, ry, rz);
    const int count = rx * ry * rz;

    std::vector<float> weights((size_t)count, 0.0f);
    int nearest = -1;
    float nearestDistance = 0.0f;
    bool any = false;
    for (int k = 0; k < rz; k++){
        for (int j = 0; j < ry; j++){
            for (int i = 0; i < rx; i++){
                const int index = lattice::pointIndex(rx, ry, i, j, k);
                Vector3 local = lattice::restPosition(rx, ry, rz, i, j, k);
                if ((size_t)index < lattice.offsets.size()) local += lattice.offsets[index];
                if ((size_t)index < lattice.dynamicOffsets.size()) local += lattice.dynamicOffsets[index];
                const float distance = (latticeModel * local - worldPoint).length();

                if (nearest < 0 || distance < nearestDistance){
                    nearest = index;
                    nearestDistance = distance;
                }
                if (worldRadius > 0.0f && distance < worldRadius){
                    const float q = distance / worldRadius;
                    weights[index] = 1.0f - q * q * (3.0f - 2.0f * q);
                    any = any || weights[index] > 0.0f;
                }
            }
        }
    }

    if (worldRadius <= 0.0f){
        if (nearest >= 0){
            poke(lattice, nearest, localImpulse);
        }
        return;
    }
    if (!any){
        return;
    }
    wakeJiggle(lattice);
    for (int index = 0; index < count; index++){
        if (weights[index] > 0.0f){
            lattice.jiggleVelocities[index] += localImpulse * weights[index];
        }
    }
}

Vector3 LatticeSystem::deformWorldPoint(Entity entity, const Vector3& worldPoint, float weight) const{
    if (entity == NULL_ENTITY || !scene->isEntityCreated(entity)){
        return worldPoint;
    }
    const LatticeComponent* lattice = scene->findComponent<LatticeComponent>(entity);
    const Transform* transform = scene->findComponent<Transform>(entity);
    if (!lattice || !transform || !lattice->enabled || !scene->isEntityActiveInHierarchy(entity)){
        return worldPoint;
    }
    if (!isInvertible(transform->modelMatrix.linear())){
        return worldPoint;
    }
    std::vector<Vector3> padded;
    lattice::buildPaddedGrid(*lattice, padded);
    return lattice::deformWorldPoint(*lattice, padded, transform->modelMatrix, worldPoint, weight);
}

bool LatticeSystem::isValidFollower(Entity follower, Entity lattice, Entity rest) const{
    if (follower == NULL_ENTITY || rest == NULL_ENTITY || lattice == NULL_ENTITY){
        return false;
    }
    if (follower == rest || follower == lattice){
        return false;
    }

    // true when `ancestor` is `entity` or one of its parents
    auto isAncestor = [this](Entity ancestor, Entity entity){
        int guard = 0;
        while (entity != NULL_ENTITY && guard++ < 100000){
            if (entity == ancestor){
                return true;
            }
            const Transform* transform = scene->findComponent<Transform>(entity);
            if (!transform){
                return false;
            }
            entity = transform->parent;
        }
        return false;
    };

    return !isAncestor(follower, rest) && !isAncestor(follower, lattice);
}

void LatticeSystem::updateTweens(double dt){
    auto lattices = scene->getComponentArray<LatticeComponent>();
    for (size_t i = 0; i < lattices->size(); i++){
        LatticeComponent& lattice = lattices->getComponentFromIndex(i);
        if (!lattice.tweening){
            continue;
        }
        const Entity entity = lattices->getEntity(i);
        if (!lattice.enabled || !scene->isEntityActiveInHierarchy(entity)){
            continue;
        }

        int rx, ry, rz;
        getResolution(lattice, rx, ry, rz);
        const size_t count = (size_t)(rx * ry * rz);
        // the resolution changed without setResolution (it resamples the tween): nothing to go on
        if (lattice.tweenFrom.size() != count || lattice.tweenTo.size() != count){
            stopTween(lattice);
            continue;
        }

        lattice.tweenTime += (float)dt;
        if (lattice.tweenTime >= lattice.tweenDuration){
            lattice.offsets = lattice.tweenTo;
            stopTween(lattice);
        }else{
            const float t = easeValue(lattice.tweenEase, lattice.tweenTime / lattice.tweenDuration);
            lattice.offsets.resize(count);
            for (size_t p = 0; p < count; p++){
                lattice.offsets[p] = lattice.tweenFrom[p] + (lattice.tweenTo[p] - lattice.tweenFrom[p]) * t;
            }
        }
        lattice.revision++;
    }
}

void LatticeSystem::stepJiggle(LatticeComponent& lattice, const Transform& transform, double dt){
    const Matrix3 linear = transform.modelMatrix.linear();
    if (!isInvertible(linear)){
        // a zero scale has no local space to move in
        resetJiggle(lattice);
        return;
    }

    int rx, ry, rz;
    getResolution(lattice, rx, ry, rz);
    const size_t count = (size_t)(rx * ry * rz);
    if (!lattice.jiggleSleeping && (lattice.dynamicOffsets.size() != count || lattice.jiggleVelocities.size() != count)){
        // the resolution changed under it
        resetJiggle(lattice);
    }

    // world acceleration of the lattice by finite differences of its position, ignored for the
    // first two frames after enabling or a teleport
    const Vector3 position = translationOf(transform.modelMatrix);
    Vector3 acceleration(0, 0, 0);
    const bool teleport = lattice.jiggleWarmup > 0 &&
        (dt > JIGGLE_MAX_FRAME || (position - lattice.jiggleLastPosition).length() > JIGGLE_TELEPORT_SIZES * maxAxisLength(linear));
    if (lattice.jiggleWarmup == 0 || teleport){
        lattice.jiggleLastVelocity = Vector3(0, 0, 0);
        lattice.jiggleWarmup = 1;
    }else{
        const Vector3 velocity = (position - lattice.jiggleLastPosition) / (float)dt;
        if (lattice.jiggleWarmup == 1){
            lattice.jiggleWarmup = 2;
        }else{
            acceleration = (velocity - lattice.jiggleLastVelocity) / (float)dt;
        }
        lattice.jiggleLastVelocity = velocity;
    }
    lattice.jiggleLastPosition = position;

    const Vector3 localAcceleration = linear.inverse(EPSILON) * acceleration;
    const float inertia = lattice.jiggleInertia;
    const float stiffness = std::max(0.0f, lattice.jiggleStiffness);
    const float damping = std::max(0.0f, lattice.jiggleDamping);
    const float maxOffset = std::max(0.0f, lattice.jiggleMaxOffset);
    const Vector3 force = localAcceleration * inertia;

    if (lattice.jiggleSleeping){
        // asleep until a push would move a point past the sleep threshold
        if (force.length() <= JIGGLE_SLEEP * std::max(stiffness, 1.0f)){
            return;
        }
        wakeJiggle(lattice);
    }

    // semi-implicit Euler, substeps of at most 1/120 s (and short enough for a stiff spring)
    double maxStep = JIGGLE_MAX_STEP;
    if (stiffness > 0.0f){
        maxStep = std::min(maxStep, 1.0 / std::sqrt((double)stiffness));
    }
    // a long frame is integrated as a short one: the spring cannot catch up in one step anyway
    const double stepTime = std::min(dt, JIGGLE_MAX_FRAME);
    const int steps = std::clamp((int)std::ceil(stepTime / maxStep), 1, JIGGLE_MAX_SUBSTEPS);
    const float h = (float)(stepTime / steps);

    std::vector<float>& weights = jiggleWeights;
    weights.assign(count, 1.0f);
    if (lattice.jiggleAnchor != LatticeJiggleAnchor::NONE){
        for (int k = 0; k < rz; k++){
            for (int j = 0; j < ry; j++){
                for (int i = 0; i < rx; i++){
                    weights[lattice::pointIndex(rx, ry, i, j, k)] =
                        anchorWeight(lattice.jiggleAnchor, lattice::restPosition(rx, ry, rz, i, j, k));
                }
            }
        }
    }

    bool settled = true;
    for (size_t p = 0; p < count; p++){
        Vector3& x = lattice.dynamicOffsets[p];
        Vector3& v = lattice.jiggleVelocities[p];
        const Vector3 push = force * weights[p];
        for (int s = 0; s < steps; s++){
            v += (x * -stiffness - v * damping - push) * h;
            x += v * h;

            const float length = x.length();
            if (length > maxOffset){
                if (length > 0.0f){
                    const Vector3 direction = x / length;
                    x = direction * maxOffset;
                    // the limit stops the outward motion
                    const float outward = v.dotProduct(direction);
                    if (outward > 0.0f){
                        v -= direction * outward;
                    }
                }else{
                    x = Vector3(0, 0, 0);
                }
            }
        }
        if (!x.isValid() || !v.isValid()){
            x = Vector3(0, 0, 0);
            v = Vector3(0, 0, 0);
        }
        if (x.length() >= JIGGLE_SLEEP || v.length() >= JIGGLE_SLEEP){
            settled = false;
        }
    }

    if (settled){
        // nothing to pack any more
        lattice.dynamicOffsets.clear();
        lattice.jiggleVelocities.clear();
        lattice.jiggleSleeping = true;
    }
    lattice.revision++;
}

void LatticeSystem::updateJiggle(double dt){
    auto lattices = scene->getComponentArray<LatticeComponent>();
    for (size_t i = 0; i < lattices->size(); i++){
        LatticeComponent& lattice = lattices->getComponentFromIndex(i);
        const Entity entity = lattices->getEntity(i);

        if (!lattice.jiggle || !lattice.enabled){
            if (lattice.jiggleWarmup != 0 || !lattice.dynamicOffsets.empty() || !lattice.jiggleVelocities.empty()){
                resetJiggle(lattice);
            }
            continue;
        }
        // after frames without a step (paused, inactive) the position history is stale: a
        // lattice moved meanwhile would read as a huge acceleration. The springs are kept.
        if (jiggleHistoryStale){
            lattice.jiggleWarmup = 0;
        }
        // an inactive lattice keeps its state, like a paused one
        if (!scene->isEntityActiveInHierarchy(entity)){
            lattice.jiggleWarmup = 0;
            continue;
        }
        const Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform){
            continue;
        }
        stepJiggle(lattice, *transform, dt);
    }
}

void LatticeSystem::updateFollowers(){
    auto followers = scene->getComponentArray<LatticeFollowerComponent>();
    if (followers->size() == 0){
        return;
    }

    auto transforms = scene->getComponentArray<Transform>();

    // parents first, so a follower placed on another one's subtree sees it moved
    std::vector<std::pair<size_t, Entity>>& order = followerOrder;
    order.clear();
    for (size_t i = 0; i < followers->size(); i++){
        const Entity entity = followers->getEntity(i);
        if (transforms->hasEntity(entity)){
            order.emplace_back(transforms->getIndex(entity), entity);
        }
    }
    std::sort(order.begin(), order.end());

    std::shared_ptr<RenderSystem> render = scene->getSystem<RenderSystem>();
    // padded grids built this frame, one per lattice (the slots and their vectors are reused)
    followerGridCount = 0;

    for (const auto& entry : order){
        const Entity entity = entry.second;
        LatticeFollowerComponent* follower = scene->findComponent<LatticeFollowerComponent>(entity);
        if (!follower || !follower->enabled || !scene->isEntityActiveInHierarchy(entity)){
            continue;
        }
        if (!follower->applyPosition && !follower->applyRotation && !follower->applyScale){
            continue;
        }

        // missing lattice or rest: left alone
        const Entity latticeEntity = follower->lattice;
        const Entity restEntity = follower->rest;
        if (latticeEntity == NULL_ENTITY || restEntity == NULL_ENTITY ||
                !scene->isEntityCreated(latticeEntity) || !scene->isEntityCreated(restEntity)){
            continue;
        }
        const LatticeComponent* lattice = scene->findComponent<LatticeComponent>(latticeEntity);
        const Transform* latticeTransform = scene->findComponent<Transform>(latticeEntity);
        const Transform* restTransform = scene->findComponent<Transform>(restEntity);
        if (!lattice || !latticeTransform || !restTransform){
            continue;
        }
        if (!isValidFollower(entity, latticeEntity, restEntity)){
            continue;
        }
        // a zero scale lattice has no space to deform in: the follower is left alone (spec §6.3)
        if (!isInvertible(latticeTransform->modelMatrix.linear())){
            continue;
        }
        // a lattice that deforms nothing (disabled, inactive) puts the follower at its rest
        const bool deforms = lattice->enabled && scene->isEntityActiveInHierarchy(latticeEntity);
        const float weight = deforms ? follower->weight : 0.0f;

        // the rest's world transform
        const Vector3 restPosition = restTransform->worldPosition;
        const Quaternion restRotation = restTransform->worldRotation;
        const Vector3 restScale = restTransform->worldScale;

        Vector3 worldPosition = restPosition;
        Quaternion worldRotation = restRotation;
        Vector3 worldScale = restScale;

        if (weight != 0.0f){
            size_t slot = 0;
            while (slot < followerGridCount && followerGrids[slot].first != latticeEntity){
                slot++;
            }
            if (slot == followerGridCount){
                if (slot == followerGrids.size()){
                    followerGrids.emplace_back();
                }
                followerGrids[slot].first = latticeEntity;
                lattice::buildPaddedGrid(*lattice, followerGrids[slot].second);
                followerGridCount++;
            }
            const std::vector<Vector3>& padded = followerGrids[slot].second;
            const Matrix4& latticeModel = latticeTransform->modelMatrix;

            worldPosition = lattice::deformWorldPoint(*lattice, padded, latticeModel, restPosition, weight);

            if (follower->applyRotation || follower->applyScale){
                const Matrix3 jacobian = lattice::worldJacobian(*lattice, padded, latticeModel, restPosition, weight);
                const Vector3 axisX = jacobian * restRotation.xAxis();
                const Vector3 axisY = jacobian * restRotation.yAxis();
                const Vector3 axisZ = jacobian * restRotation.zAxis();

                if (follower->applyRotation){
                    // Gram-Schmidt of the deformed rest basis; a degenerate one keeps the rest rotation
                    const float lengthX = axisX.length();
                    if (lengthX > EPSILON){
                        const Vector3 e1 = axisX / lengthX;
                        Vector3 e2 = axisY - e1 * axisY.dotProduct(e1);
                        const float lengthY = e2.length();
                        if (lengthY > EPSILON){
                            e2 = e2 / lengthY;
                            const Vector3 e3 = e1.crossProduct(e2);
                            worldRotation = Quaternion(e1, e2, e3).normalized();
                        }
                    }
                }
                if (follower->applyScale){
                    worldScale = Vector3(restScale.x * axisX.length(), restScale.y * axisY.length(), restScale.z * axisZ.length());
                }
            }
        }

        // into the follower's parent space; channels not applied are left as they are
        Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform){
            continue;
        }
        const Transform* parent = transform->parent != NULL_ENTITY ? scene->findComponent<Transform>(transform->parent) : nullptr;

        Vector3 position = transform->position;
        Quaternion rotation = transform->rotation;
        Vector3 scale = transform->scale;

        if (follower->applyPosition){
            if (parent){
                const Matrix4 parentInverse = parent->modelMatrix.inverse();
                if (parentInverse.isValid()){
                    position = parentInverse * worldPosition;
                }
            }else{
                position = worldPosition;
            }
        }
        if (follower->applyRotation){
            rotation = parent ? (parent->worldRotation.inverse() * worldRotation).normalized() : worldRotation;
        }
        if (follower->applyScale){
            if (parent){
                const Vector3& parentScale = parent->worldScale;
                if (parentScale.x != 0.0f) scale.x = worldScale.x / parentScale.x;
                if (parentScale.y != 0.0f) scale.y = worldScale.y / parentScale.y;
                if (parentScale.z != 0.0f) scale.z = worldScale.z / parentScale.z;
            }else{
                scale = worldScale;
            }
        }

        if (!position.isValid() || !isFinite(rotation) || !scale.isValid()){
            continue;
        }
        if (position == transform->position && rotation == transform->rotation && scale == transform->scale){
            continue;
        }
        transform->position = position;
        transform->rotation = rotation;
        transform->scale = scale;

        // the follower and its subtree get their world matrices this frame: the branch that
        // follows it in the Transform array (parents before children), which ends at the
        // first transform that does not descend from the follower
        const size_t start = transforms->getIndex(entity);
        for (size_t t = start; t < transforms->size(); t++){
            Transform& current = transforms->getComponentFromIndex(t);
            const Entity currentEntity = transforms->getEntity(t);
            if (t != start){
                bool inBranch = false;
                Entity ancestor = current.parent;
                while (ancestor != NULL_ENTITY){
                    if (ancestor == entity){
                        inBranch = true;
                        break;
                    }
                    const Transform* ancestorTransform = transforms->findComponent(ancestor);
                    ancestor = ancestorTransform ? ancestorTransform->parent : NULL_ENTITY;
                }
                if (!inBranch){
                    break;
                }
            }
            current.needUpdate = true;
            render->updateTransform(current, currentEntity);
        }
    }
}

void LatticeSystem::lateUpdate(){
    if (!paused){
        const double dt = jiggleTime;
        jiggleTime = 0.0;
        if (dt > 0.0){
            updateJiggle(dt);
            jiggleHistoryStale = false;
        }
    }else{
        jiggleHistoryStale = true;
    }

    updateFollowers();
}

void LatticeSystem::load(){
}

void LatticeSystem::draw(){
}

void LatticeSystem::destroy(){
}

void LatticeSystem::update(double dt){
    if (paused){
        return;
    }

    updateTweens(dt);

    jiggleTime += dt;
}
