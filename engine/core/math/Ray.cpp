// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Ray.h"
#include "physics/ContactTracker.h"

#include "Scene.h"
#include "component/MeshComponent.h"
#include "registry/TagManager.h"
#ifdef JASPERENGINE_PHYSICS_2D
#include "object/physics/Rigidbody2D.h"
#include "util/Box2DAux.h"
#endif
#ifdef JASPERENGINE_PHYSICS_3D
#include "object/physics/Rigidbody.h"
#include "util/JoltPhysicsAux.h"
#endif
#include <algorithm>
#include <stdlib.h>

using namespace jasperengine;


const RayReturn Ray::NO_HIT( {false, -1, Vector3::ZERO, Vector3::ZERO, NULL_ENTITY, 0} );

Ray::Ray(){

}

Ray::Ray(const Ray &ray){
    (*this) = ray;
}

Ray::Ray(Vector3 origin, Vector3 direction){
    this->origin = origin;
    this->direction = direction;
}

Ray& Ray::operator=(const Ray &r){

    this->origin = r.origin;
    this->direction = r.direction;

    return *this;
}

Vector3 Ray::operator*(float t){
    return getPoint(t);
}

void Ray::setOrigin(Vector3 origin){
    this->origin = origin;
}

Vector3 Ray::getOrigin() const{
    return origin;
}

void Ray::setDirection(Vector3 direction){
    this->direction = direction;
}

Vector3 Ray::getDirection() const{
    return direction;
}

Vector3 Ray::getPoint(float distance) const{
    return Vector3(origin + (direction * distance));
}

RayReturn Ray::intersects(const Plane& plane) const{
    float denom = plane.normal.dotProduct(getDirection());

    if (abs(denom) < std::numeric_limits<float>::epsilon()) {
        // when plane is parallel
        return NO_HIT;
    }else{
        float nom = plane.normal.dotProduct(getOrigin()) + plane.d;
        float dist = -(nom/denom);
        if ((dist >= 0) && (dist <= 1)) // anything beyond this length will not be a hit
            return {true, dist, getPoint(dist), plane.normal, NULL_ENTITY, 0};
    }

    return NO_HIT;
}

RayReturn Ray::intersects(const AABB& box) const{

    if (box.isNull()) return NO_HIT;
    if (box.isInfinite()) return NO_HIT;

    float lowt = 0.0f;
    float t;
    bool hit = false;
    Vector3 hitpoint;
    const Vector3& min = box.getMinimum();
    const Vector3& max = box.getMaximum();
    const Vector3& rayorig = getOrigin();
    const Vector3& raydir = getDirection();

    // inside box
    if ( rayorig > min && rayorig < max ) {
        return {true, 0, getPoint(0), Vector3::ZERO, NULL_ENTITY, 0};
    }

    // Min x
    if (rayorig.x <= min.x && raydir.x > 0) {
        t = (min.x - rayorig.x) / raydir.x;

        hitpoint = rayorig + raydir * t;
        if (hitpoint.y >= min.y && hitpoint.y <= max.y && hitpoint.z >= min.z && hitpoint.z <= max.z && (!hit || t < lowt)) {
            hit = true;
            lowt = t;
        }
    }
    // Max x
    if (rayorig.x >= max.x && raydir.x < 0) {
        t = (max.x - rayorig.x) / raydir.x;

        hitpoint = rayorig + raydir * t;
        if (hitpoint.y >= min.y && hitpoint.y <= max.y && hitpoint.z >= min.z && hitpoint.z <= max.z && (!hit || t < lowt)) {
            hit = true;
            lowt = t;
        }
    }
    // Min y
    if (rayorig.y <= min.y && raydir.y > 0) {
        t = (min.y - rayorig.y) / raydir.y;

        hitpoint = rayorig + raydir * t;
        if (hitpoint.x >= min.x && hitpoint.x <= max.x && hitpoint.z >= min.z && hitpoint.z <= max.z && (!hit || t < lowt)) {
            hit = true;
            lowt = t;
        }
    }
    // Max y
    if (rayorig.y >= max.y && raydir.y < 0) {
        t = (max.y - rayorig.y) / raydir.y;

        hitpoint = rayorig + raydir * t;
        if (hitpoint.x >= min.x && hitpoint.x <= max.x && hitpoint.z >= min.z && hitpoint.z <= max.z && (!hit || t < lowt)) {
            hit = true;
            lowt = t;
        }
    }
    // Min z
    if (rayorig.z <= min.z && raydir.z > 0) {
        t = (min.z - rayorig.z) / raydir.z;

        hitpoint = rayorig + raydir * t;
        if (hitpoint.x >= min.x && hitpoint.x <= max.x && hitpoint.y >= min.y && hitpoint.y <= max.y && (!hit || t < lowt)) {
            hit = true;
            lowt = t;
        }
    }
    // Max z
    if (rayorig.z >= max.z && raydir.z < 0) {
        t = (max.z - rayorig.z) / raydir.z;

        hitpoint = rayorig + raydir * t;
        if (hitpoint.x >= min.x && hitpoint.x <= max.x && hitpoint.y >= min.y && hitpoint.y <= max.y && (!hit || t < lowt)) {
            hit = true;
            lowt = t;
        }
    }

    if (hit)
        if ((lowt >= 0) && (lowt <= 1)){ // anything beyond this length will not be a hit

            Vector3 point = getPoint(lowt);
            Vector3 normal = Vector3::ZERO;

            if (point.x == min.x)
                normal = Vector3(-1, 0, 0); // Left face
            if (point.x == max.x)
                normal = Vector3(1, 0, 0); // Right face
            if (point.y == min.y)
                normal = Vector3(0, -1, 0); // Bottom face
            if (point.y == max.y)
                normal = Vector3(0, 1, 0); // Top face
            if (point.z == min.z)
                normal = Vector3(0, 0, -1); // Front face
            if (point.z == max.z)
                normal = Vector3(0, 0, 1); // Back face

            return {true, lowt, point, normal, NULL_ENTITY, 0};
        }

    return NO_HIT;
}

RayReturn Ray::intersects(const OBB& obb) const{
    // First transform the ray into the OBB's local space
    Vector3 localOrigin = origin - obb.getCenter();

    // Get the OBB axes
    Vector3 dirX = obb.getAxisX();
    Vector3 dirY = obb.getAxisY();
    Vector3 dirZ = obb.getAxisZ();

    // Convert ray origin and direction to OBB space
    Vector3 obbSpaceOrigin(
        localOrigin.dotProduct(dirX),
        localOrigin.dotProduct(dirY),
        localOrigin.dotProduct(dirZ)
    );

    Vector3 obbSpaceDirection(
        direction.dotProduct(dirX),
        direction.dotProduct(dirY),
        direction.dotProduct(dirZ)
    );

    // Now we can treat it as an AABB intersection test in the OBB's local space
    Vector3 halfExtents = obb.getHalfExtents();
    Vector3 min = -halfExtents;
    Vector3 max = halfExtents;

    // Ray-AABB intersection algorithm (slab method)
    float tmin = -std::numeric_limits<float>::infinity();
    float tmax = std::numeric_limits<float>::infinity();

    // Track which axis/face causes the intersection
    int minAxis = -1;
    bool isMinFace = true; // true if it's the "min" face of the axis, false if it's the "max" face

    // Check intersection with each pair of planes
    for (int i = 0; i < 3; ++i) {
        if (std::abs(obbSpaceDirection[i]) < 1e-6f) {
            // Ray is parallel to the slab, check if ray origin is inside slab
            if (obbSpaceOrigin[i] < min[i] || obbSpaceOrigin[i] > max[i]) {
                return NO_HIT;
            }
        } else {
            // Compute intersection with the planes
            float ood = 1.0f / obbSpaceDirection[i];
            float t1 = (min[i] - obbSpaceOrigin[i]) * ood;
            float t2 = (max[i] - obbSpaceOrigin[i]) * ood;

            // Make t1 the intersection with the near plane, t2 with far plane
            bool flipped = false;
            if (t1 > t2) {
                std::swap(t1, t2);
                flipped = true;
            }

            // Update tmin and tmax
            if (t1 > tmin) {
                tmin = t1;
                minAxis = i;
                isMinFace = !flipped;
            }
            tmax = std::min(tmax, t2);

            // Exit early if no intersection
            if (tmin > tmax) {
                return NO_HIT;
            }
        }
    }

    // If tmax < 0, the ray is intersecting the OBB but the whole OBB is behind the ray origin
    if (tmax < 0) {
        return NO_HIT;
    }

    // If tmin < 0, the ray origin is inside the OBB, use tmax and the exit normal
    float t = (tmin >= 0) ? tmin : tmax;
    if (t > 1.0f) { // Beyond the ray length
        return NO_HIT;
    }

    // Calculate the hit point in world space
    Vector3 hitPoint = getPoint(t);

    // Determine the normal in OBB space
    Vector3 obbNormal = Vector3::ZERO;
    if (minAxis == 0) {
        obbNormal = isMinFace ? Vector3(-1, 0, 0) : Vector3(1, 0, 0);
    } else if (minAxis == 1) {
        obbNormal = isMinFace ? Vector3(0, -1, 0) : Vector3(0, 1, 0);
    } else if (minAxis == 2) {
        obbNormal = isMinFace ? Vector3(0, 0, -1) : Vector3(0, 0, 1);
    }

    // Transform the normal back to world space
    Vector3 worldNormal = (
        obbNormal.x * dirX +
        obbNormal.y * dirY +
        obbNormal.z * dirZ
    );
    worldNormal.normalize();

    return {true, t, hitPoint, worldNormal, NULL_ENTITY, 0};
}

RayReturn Ray::intersects(const Sphere& sphere) const{
    Vector3 rayorig = origin - sphere.center;
    float radius = sphere.radius;

    float a = direction.dotProduct(direction);
    float b = 2 * rayorig.dotProduct(direction);
    float c = rayorig.dotProduct(rayorig) - radius * radius;

    // determinant
    float d = (b * b) - (4 * a * c);
    if (d >= 0){
        float t = ( -b - sqrt(d) ) / (2 * a);
        if (t < 0){
            t = ( -b + sqrt(d) ) / (2 * a);
        }
        // the sphere lies behind the origin or past the end of the ray
        if (t < 0 || t > 1){
            return NO_HIT;
        }

        Vector3 point = getPoint(t);
        Vector3 normal = (point - sphere.center).normalize();

        return {true, t, point, normal, NULL_ENTITY, 0};
    }

    return NO_HIT;
}

#ifdef JASPERENGINE_PHYSICS_2D
RayReturn Ray::intersects(const Rigidbody2D& body) const{
    Rigidbody2DComponent& bodycomp = body.getComponent<Rigidbody2DComponent>();

    if (!b2Body_IsValid(bodycomp.body)){
        return NO_HIT;
    }

    float ptmScale = body.getPointsToMeterScale();

    Vector3 end = origin + direction;
    b2Vec2 bOrigin = {origin.x / ptmScale, origin.y / ptmScale};
    b2Vec2 bEnd = {end.x / ptmScale, end.y / ptmScale};

    b2Vec2 translation = b2Sub(bEnd, bOrigin);

    const int shapesCount = b2Body_GetShapeCount(bodycomp.body);
    if (shapesCount == 0){
        return NO_HIT;
    }
    std::vector<b2ShapeId> shapeIds(shapesCount);
    int returnCount = b2Body_GetShapes(bodycomp.body, &shapeIds.front(), shapesCount);

    float closestFraction = FLT_MAX;
    b2Vec2 intersectionPoint = {0, 0};
    b2Vec2 intersectionNormal = {0, 0};
    size_t colliderIndex = 0;
    for (int i = 0; i < returnCount; ++i){
        b2ShapeId shapeid = shapeIds[i];
        const b2RayCastInput rayCastInput = {bOrigin, translation, 1.0};
        b2CastOutput castOutput = b2Shape_RayCast(shapeid, &rayCastInput);

        // the nearest collider, whatever order the body lists them in
        if (castOutput.hit && castOutput.fraction < closestFraction){
            closestFraction = castOutput.fraction;
            intersectionPoint = castOutput.point;
            intersectionNormal = castOutput.normal;
            colliderIndex = reinterpret_cast<size_t>(b2Shape_GetUserData(shapeid));
        }
    }

    if (closestFraction >= 0 && closestFraction <= 1){
        Vector3 point = Vector3(intersectionPoint.x * ptmScale, intersectionPoint.y * ptmScale, 0);
        Vector3 normal = Vector3(intersectionNormal.x, intersectionNormal.y, 0);

        return {true, closestFraction, point, normal, body.getEntity(), colliderIndex};
    }

    return NO_HIT;
}

RayReturn Ray::intersects(const Rigidbody2D& body, size_t shape) const{
    Rigidbody2DComponent& bodycomp = body.getComponent<Rigidbody2DComponent>();

    if (!findCollider2D(bodycomp, (int)shape)){
        return NO_HIT;
    }

    // every Box2D shape of the collider: the convex pieces of a polygon, a chain's segments
    std::vector<b2ShapeId> shapeIds = body.getBox2DShapes(shape);
    if (shapeIds.empty()){
        return NO_HIT;
    }

    float ptmScale = body.getPointsToMeterScale();

    Vector3 end = origin + direction;
    b2Vec2 bOrigin = {origin.x / ptmScale, origin.y / ptmScale};
    b2Vec2 bEnd = {end.x / ptmScale, end.y / ptmScale};

    b2Vec2 translation = b2Sub(bEnd, bOrigin);

    const b2RayCastInput rayCastInput = {bOrigin, translation, 1.0};
    bool hit = false;
    b2CastOutput closest = {};
    for (const b2ShapeId& shapeid : shapeIds){
        b2CastOutput castOutput = b2Shape_RayCast(shapeid, &rayCastInput);
        if (castOutput.hit && (!hit || castOutput.fraction < closest.fraction)){
            closest = castOutput;
            hit = true;
        }
    }

    if (hit){
        Vector3 point = Vector3(closest.point.x * ptmScale, closest.point.y * ptmScale, 0);
        Vector3 normal = Vector3(closest.normal.x, closest.normal.y, 0);

        return {true, closest.fraction, point, normal, body.getEntity(), shape};
    }

    return NO_HIT;
}
#endif

#ifdef JASPERENGINE_PHYSICS_3D
RayReturn Ray::intersects(const Rigidbody& body) const{
    RigidbodyComponent& bodycomp = body.getComponent<RigidbodyComponent>();
    std::shared_ptr<PhysicsSystem> physicsSystem = body.getScene()->getSystem<PhysicsSystem>();

    if (!bodycomp.body.IsInvalid()){
        JPH::RayCast ray(JPH::Vec3(origin.x, origin.y, origin.z), JPH::Vec3(direction.x, direction.y, direction.z));
        JPH::SubShapeIDCreator id_creator;
        JPH::RayCastResult hit;

        JPH::ShapeRefC shapeRef = physicsSystem->getBodyInterface3D().GetShape(bodycomp.body);
        // trigger colliders of a body with solid ones are on their own body (PhysicsSystem): the nearer
        if (!bodycomp.triggerBody.IsInvalid()){
            JPH::ShapeRefC triggers = physicsSystem->getBodyInterface3D().GetShape(bodycomp.triggerBody);
            JPH::RayCastResult solidHit;
            const bool solid = shapeRef->CastRay(ray, id_creator, solidHit);
            JPH::RayCastResult triggerHit;
            if (triggers && triggers->CastRay(ray, id_creator, triggerHit) && (!solid || triggerHit.mFraction < solidHit.mFraction)){
                shapeRef = triggers;
            }
        }

        if (shapeRef->CastRay(ray, id_creator, hit)){
            JPH::Vec3 normal = shapeRef->GetSurfaceNormal(hit.mSubShapeID2, ray.GetPointOnRay(hit.mFraction));

            size_t colliderIndex = shapeRef->GetSubShapeUserData(hit.mSubShapeID2);

            return {true, hit.mFraction, getPoint(hit.mFraction), Vector3(normal.GetX(), normal.GetY(), normal.GetZ()), body.getEntity(), colliderIndex};
        }
    }

    return NO_HIT;
}

RayReturn Ray::intersects(const Rigidbody& body, size_t shape) const{
    RigidbodyComponent& bodycomp = body.getComponent<RigidbodyComponent>();

    if (!bodycomp.body.IsInvalid()){
        JPH::RayCast ray(JPH::Vec3(origin.x, origin.y, origin.z), JPH::Vec3(direction.x, direction.y, direction.z));
        JPH::SubShapeIDCreator id_creator;
        JPH::RayCastResult hit;

        if (shape < bodycomp.colliders.size()){
            if (bodycomp.colliders[shape].shape->CastRay(ray, id_creator, hit)){
                JPH::Vec3 normal = bodycomp.colliders[shape].shape->GetSurfaceNormal(hit.mSubShapeID2, ray.GetPointOnRay(hit.mFraction));

                return {true, hit.mFraction, getPoint(hit.mFraction), Vector3(normal.GetX(), normal.GetY(), normal.GetZ()), body.getEntity(), shape};
            }
        }
    }

    return NO_HIT;
}
#endif

RayReturn Ray::intersects(Scene* scene, RayFilter raytest) const{
    return intersects(scene, raytest, false);
}

RayReturn Ray::intersects(Scene* scene, RayFilter raytest, bool onlyStatic) const{
    return intersects(scene, raytest, onlyStatic, (uint16_t)~0u, (uint16_t)~0u);
}

RayReturn Ray::intersects(Scene* scene, RayFilter raytest, uint16_t categoryBits, uint16_t maskBits) const{
    return intersects(scene, raytest, false, categoryBits, maskBits);
}

RayReturn Ray::intersects(Scene* scene, RayFilter raytest, Entity ignoreEntity) const{
    const std::vector<Entity> ignoreEntities{ignoreEntity};
    return cast(scene, raytest, -1, false, (uint16_t)~0u, (uint16_t)~0u, &ignoreEntities, LAYER_MASK_DEFAULT_RAYCAST, nullptr);
}

RayReturn Ray::intersects(Scene* scene, RayFilter raytest, const std::vector<Entity>& ignoreEntities) const{
    return cast(scene, raytest, -1, false, (uint16_t)~0u, (uint16_t)~0u, &ignoreEntities, LAYER_MASK_DEFAULT_RAYCAST, nullptr);
}

RayReturn Ray::intersects(Scene* scene, RayFilter raytest, bool onlyStatic, uint16_t categoryBits, uint16_t maskBits) const{
    return cast(scene, raytest, -1, onlyStatic, categoryBits, maskBits, nullptr, LAYER_MASK_DEFAULT_RAYCAST, nullptr);
}

RayReturn Ray::intersects(Scene* scene, RayFilter raytest, bool onlyStatic, uint16_t categoryBits, uint16_t maskBits, Entity ignoreEntity) const{
    const std::vector<Entity> ignoreEntities{ignoreEntity};
    return cast(scene, raytest, -1, onlyStatic, categoryBits, maskBits, &ignoreEntities, LAYER_MASK_DEFAULT_RAYCAST, nullptr);
}

RayReturn Ray::intersects(Scene* scene, RayFilter raytest, bool onlyStatic, uint16_t categoryBits, uint16_t maskBits, const std::vector<Entity>& ignoreEntities) const{
    return cast(scene, raytest, -1, onlyStatic, categoryBits, maskBits, &ignoreEntities, LAYER_MASK_DEFAULT_RAYCAST, nullptr);
}

namespace{

    bool isIgnoredEntity(const std::vector<Entity>* ignoreEntities, Entity entity){
        return ignoreEntities && std::find(ignoreEntities->begin(), ignoreEntities->end(), entity) != ignoreEntities->end();
    }

    // Nearest first, then a single hit per collider (a chain or mesh collider can report
    // several), keeping its nearest
    void sortAndDedupeHits(std::vector<RayReturn>& hits){
        std::stable_sort(hits.begin(), hits.end(), [](const RayReturn& a, const RayReturn& b){
            return a.distance < b.distance;
        });
        size_t kept = 0;
        for (size_t i = 0; i < hits.size(); i++){
            bool repeated = false;
            for (size_t j = 0; j < kept; j++){
                if (hits[j].body == hits[i].body && hits[j].colliderIndex == hits[i].colliderIndex){
                    repeated = true;
                    break;
                }
            }
            if (!repeated){
                hits[kept++] = hits[i];
            }
        }
        hits.resize(kept);
    }

}

RayReturn Ray::cast(Scene* scene, RayFilter raytest, int broadPhaseLayer3D, bool onlyStatic, uint16_t categoryBits, uint16_t maskBits, const std::vector<Entity>* ignoreEntities, uint32_t layerMask, std::vector<RayReturn>* allHits) const{
    if (allHits){
        allHits->clear();
    }
    if (!scene || layerMask == LAYER_MASK_NOTHING){
        return NO_HIT;
    }

    if (raytest == RayFilter::BOUNDS){
        RayReturn closest = NO_HIT;
        auto meshes = scene->getComponentArray<MeshComponent>();

        for (size_t i = 0; i < meshes->size(); i++){
            Entity entity = meshes->getEntity(i);
            if (isIgnoredEntity(ignoreEntities, entity)){
                continue;
            }
            if (layerMask != LAYER_MASK_EVERYTHING && ((layerMask >> scene->getEntityLayer(entity)) & 1u) == 0){
                continue;
            }

            RayReturn hit = intersects(meshes->getComponentFromIndex(i).worldAABB);
            if (hit){
                hit.body = entity;
                if (!closest || hit.distance < closest.distance){
                    closest = hit;
                }
                if (allHits){
                    allHits->push_back(hit);
                }
            }
        }
        if (allHits){
            sortAndDedupeHits(*allHits);
        }
        return closest;
    }

#ifdef JASPERENGINE_PHYSICS_2D
    if (raytest == RayFilter::BODY_2D){
        std::shared_ptr<PhysicsSystem> physicsSystem = scene->getSystem<PhysicsSystem>();
        b2WorldId world = physicsSystem->getWorld2D();

        // The Box2D world is created lazily; with no 2D bodies in the scene there is nothing to hit.
        if (!b2World_IsValid(world)){
            return NO_HIT;
        }

        float ptmScale = physicsSystem->getPointsToMeterScale2D();

        Vector3 end = origin + direction;
        b2Vec2 bOrigin = {origin.x / ptmScale, origin.y / ptmScale};
        b2Vec2 bEnd = {end.x / ptmScale, end.y / ptmScale};

        b2Vec2 translation = b2Sub(bEnd, bOrigin);

        // The layer is tested in Box2D's tree walk, the shape's mask against categoryBits by
        // Box2D too, and the shape's category against maskBits in the callback
        std::vector<Box2DWorldRayCastOutput> outputs;

        Box2DWorldRayCastContext context;
        context.outputs = allHits ? &outputs : nullptr;
        context.onlyStatic = onlyStatic;
        context.ignoreEntities = ignoreEntities;
        context.maskBits = maskBits;

        b2World_CastRay(world, bOrigin, translation, Box2DLayers::makeQueryFilter(categoryBits, layerMask), Box2DAux::CastCallback, &context);

        auto toRayReturn = [&](const Box2DWorldRayCastOutput& output) -> RayReturn {
            Entity entity = static_cast<Entity>(reinterpret_cast<uintptr_t>(b2Body_GetUserData(output.bodyId)));
            size_t colliderIndex = reinterpret_cast<size_t>(b2Shape_GetUserData(output.shapeId));
            Vector3 point = Vector3(output.point.x * ptmScale, output.point.y * ptmScale, 0);
            Vector3 normal = Vector3(output.normal.x, output.normal.y, 0);
            return {true, output.fraction, point, normal, entity, colliderIndex};
        };

        if (allHits){
            allHits->reserve(outputs.size());
            for (const Box2DWorldRayCastOutput& output : outputs){
                if (output.fraction >= 0 && output.fraction <= 1){
                    allHits->push_back(toRayReturn(output));
                }
            }
            sortAndDedupeHits(*allHits);
            return allHits->empty() ? NO_HIT : allHits->front();
        }

        if (context.hit && context.closest.fraction >= 0 && context.closest.fraction <= 1){
            return toRayReturn(context.closest);
        }

        return NO_HIT;
    }
#endif

#ifdef JASPERENGINE_PHYSICS_3D
    if (raytest == RayFilter::BODY_3D){
        if (broadPhaseLayer3D >= MAX_BROADPHASELAYER_3D){
            return NO_HIT;
        }

        std::shared_ptr<PhysicsSystem> physicsSystem = scene->getSystem<PhysicsSystem>();
        // no 3D world made yet (no body ever): nothing to hit
        JPH::PhysicsSystem* world = physicsSystem->hasWorld3D() ? physicsSystem->getWorld3D() : nullptr;

        if (world){
            JPH::RayCast ray(JPH::Vec3(origin.x, origin.y, origin.z), JPH::Vec3(direction.x, direction.y, direction.z));

            // layer, category and mask are tested per body in Jolt's broad phase walk
            const JoltQueryObjectLayerFilter objectLayerFilter(physicsSystem->getObjectLayerTable3D(), layerMask, categoryBits, maskBits);
            const OnlyStaticBodyFilter bodyFilter(onlyStatic, ignoreEntities);
            const JPH::BroadPhaseLayerFilter anyBroadPhaseLayer;
            const JPH::SpecifiedBroadPhaseLayerFilter specifiedBroadPhaseLayer(JPH::BroadPhaseLayer(broadPhaseLayer3D < 0 ? 0 : (uint8_t)broadPhaseLayer3D));
            const JPH::BroadPhaseLayerFilter& broadPhaseFilter = broadPhaseLayer3D < 0
                ? anyBroadPhaseLayer
                : static_cast<const JPH::BroadPhaseLayerFilter&>(specifiedBroadPhaseLayer);

            auto toRayReturn = [&](const JPH::BodyID& bodyId, const JPH::SubShapeID& subShapeId, float fraction) -> RayReturn {
                JPH::Vec3 normal = JPH::Vec3::sZero();
                Entity entity = NULL_ENTITY;
                size_t colliderIndex = 0;
                JPH::BodyLockRead lock(physicsSystem->getBodyLockInterface3D(), bodyId);
                if (lock.Succeeded()){
                    const JPH::Body &hit_body = lock.GetBody();
                    normal = hit_body.GetWorldSpaceSurfaceNormal(subShapeId, ray.GetPointOnRay(fraction));
                    entity = PhysicsCallbacks::bodyEntity(hit_body.GetUserData());
                    colliderIndex = hit_body.GetShape()->GetSubShapeUserData(subShapeId);
                }
                return {true, fraction, getPoint(fraction), Vector3(normal.GetX(), normal.GetY(), normal.GetZ()), entity, colliderIndex};
            };

            if (allHits){
                JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
                physicsSystem->getNarrowPhaseQuery3D().CastRay(JPH::RRayCast(ray), JPH::RayCastSettings(), collector, broadPhaseFilter, objectLayerFilter, bodyFilter);
                allHits->reserve(collector.mHits.size());
                for (const JPH::RayCastResult& hit : collector.mHits){
                    allHits->push_back(toRayReturn(hit.mBodyID, hit.mSubShapeID2, hit.mFraction));
                }
                sortAndDedupeHits(*allHits);
                return allHits->empty() ? NO_HIT : allHits->front();
            }

            JPH::RayCastResult hit;
            if (physicsSystem->getNarrowPhaseQuery3D().CastRay(JPH::RRayCast(ray), hit, broadPhaseFilter, objectLayerFilter, bodyFilter)){
                return toRayReturn(hit.mBodyID, hit.mSubShapeID2, hit.mFraction);
            }
        }
    }
#endif

    return NO_HIT;
}

#ifdef JASPERENGINE_PHYSICS_3D
RayReturn Ray::intersects(Scene* scene, uint8_t broadPhaseLayer3D) const{
    return intersects(scene, broadPhaseLayer3D, (uint16_t)~0u, (uint16_t)~0u);
}

RayReturn Ray::intersects(Scene* scene, uint8_t broadPhaseLayer3D, uint16_t categoryBits, uint16_t maskBits) const{
    return cast(scene, RayFilter::BODY_3D, broadPhaseLayer3D, false, categoryBits, maskBits, nullptr, LAYER_MASK_DEFAULT_RAYCAST, nullptr);
}

RayReturn Ray::intersects(Scene* scene, uint8_t broadPhaseLayer3D, Entity ignoreEntity) const{
    const std::vector<Entity> ignoreEntities{ignoreEntity};
    return cast(scene, RayFilter::BODY_3D, broadPhaseLayer3D, false, (uint16_t)~0u, (uint16_t)~0u, &ignoreEntities, LAYER_MASK_DEFAULT_RAYCAST, nullptr);
}

RayReturn Ray::intersects(Scene* scene, uint8_t broadPhaseLayer3D, const std::vector<Entity>& ignoreEntities) const{
    return cast(scene, RayFilter::BODY_3D, broadPhaseLayer3D, false, (uint16_t)~0u, (uint16_t)~0u, &ignoreEntities, LAYER_MASK_DEFAULT_RAYCAST, nullptr);
}

RayReturn Ray::intersects(Scene* scene, uint8_t broadPhaseLayer3D, uint16_t categoryBits, uint16_t maskBits, Entity ignoreEntity) const{
    const std::vector<Entity> ignoreEntities{ignoreEntity};
    return cast(scene, RayFilter::BODY_3D, broadPhaseLayer3D, false, categoryBits, maskBits, &ignoreEntities, LAYER_MASK_DEFAULT_RAYCAST, nullptr);
}

RayReturn Ray::intersects(Scene* scene, uint8_t broadPhaseLayer3D, uint16_t categoryBits, uint16_t maskBits, const std::vector<Entity>& ignoreEntities) const{
    return cast(scene, RayFilter::BODY_3D, broadPhaseLayer3D, false, categoryBits, maskBits, &ignoreEntities, LAYER_MASK_DEFAULT_RAYCAST, nullptr);
}
#endif
