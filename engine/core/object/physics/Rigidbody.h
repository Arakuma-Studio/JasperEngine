// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef RIGIDBODY_H
#define RIGIDBODY_H

#ifndef JASPERENGINE_PHYSICS_3D
#error "Rigidbody needs 3D Physics, enabled in Project Settings > Build"
#endif

#include "EntityHandle.h"
#include "math/Vector2.h"
#include "component/RigidbodyComponent.h"
#include "physics/PhysicsMaterial.h"

#include "Jolt/Jolt.h"
#include "Jolt/Physics/Body/Body.h"
#include "Jolt/Physics/Body/BodyInterface.h"

namespace jasperengine{

    class GameObject;

    class JASPERENGINE_API Rigidbody: public EntityHandle{
    protected:
        void checkBody(const RigidbodyComponent& body) const;

        void applyAllowedDOFs(JPH::EAllowedDOFs allowedDOFs);

        // the collider at index; out of range: an error (a Lua error)
        Collider3D& colliderAt(size_t index) const;
        // a collider's geometry changed: its shape is made again, the body's swapped (D13)
        void colliderChanged(size_t index);
        // a trigger or material change: the body's shape and mass again, no leaf rebuilt
        void collidersChanged();
        // the mass fields changed: applied now when the body lives (else at its creation)
        void massChanged();
        // the body fields (damping...): applied to the live body
        void parametersChanged();

        const JPH::BodyLockInterface& getBodyLockInterface() const;
        JPH::BodyInterface& getBodyInterface() const;

    public:

        // component checkbox (Unity Behaviour.enabled): a disabled Rigidbody is skipped by its system
        void setEnabled(bool enabled);
        bool isEnabled() const;
        Rigidbody(Scene* scene, Entity entity);
        virtual ~Rigidbody();

        // the entity has a RigidbodyComponent (a contact's other side may be a
        // CharacterController's inner body, which has none)
        bool isValid() const;

        Rigidbody(const Rigidbody& rhs);
        Rigidbody& operator=(const Rigidbody& rhs);
        Rigidbody(Rigidbody&& rhs) noexcept;
        Rigidbody& operator=(Rigidbody&& rhs) noexcept;

        const JPH::Body& getJoltBody() const;
        JPH::Body& getJoltBodyWrite() const;
        JPH::Body* getJoltBodyNoLock() const;

        GameObject getAttachedObject();

        void load();

        int createBoxCollider(float width, float height, float depth);
        int createBoxCollider(Vector3 position, Quaternion rotation, float width, float height, float depth);
        int createSphereCollider(float radius);
        int createSphereCollider(Vector3 position, Quaternion rotation, float radius);
        int createCapsuleCollider(float halfHeight, float radius);
        int createCapsuleCollider(Vector3 position, Quaternion rotation, float halfHeight, float radius);
        int createTaperedCapsuleCollider(float halfHeight, float topRadius, float bottomRadius);
        int createTaperedCapsuleCollider(Vector3 position, Quaternion rotation, float halfHeight, float topRadius, float bottomRadius);
        int createCylinderCollider(float halfHeight, float radius);
        int createCylinderCollider(Vector3 position, Quaternion rotation, float halfHeight, float radius);
        int createConvexHullCollider();
        int createConvexHullCollider(std::vector<Vector3> vertices);
        int createConvexHullCollider(Vector3 position, Quaternion rotation, std::vector<Vector3> vertices);
        int createMeshCollider();
        int createMeshCollider(std::vector<Vector3> vertices, std::vector<uint32_t> indices);
        int createMeshCollider(Vector3 position, Quaternion rotation, std::vector<Vector3> vertices, std::vector<uint32_t> indices);
        // the 16-bit indices of before, widened
        int createMeshCollider(std::vector<Vector3> vertices, std::vector<uint16_t> indices);
        int createMeshCollider(Vector3 position, Quaternion rotation, std::vector<Vector3> vertices, std::vector<uint16_t> indices);
        int createHeightFieldCollider();
        int createHeightFieldCollider(unsigned int samplesSize);

        size_t getNumColliders() const;
        size_t getColliderCount() const;
        Collider3DType getColliderType(size_t index) const;

        // Collider edits (index from 0): that collider's shape is made again and the body's shape
        // swapped at the next step; the body keeps its id, velocities, joints and other contacts.
        // false when the index is out of range (removeCollider); later colliders shift down.
        bool removeCollider(size_t index);
        void removeAllColliders();
        void setColliderPosition(size_t index, Vector3 position);
        Vector3 getColliderPosition(size_t index) const;
        void setColliderRotation(size_t index, Quaternion rotation);
        Quaternion getColliderRotation(size_t index) const;
        // BOX: width, height, depth
        void setColliderBoxSize(size_t index, Vector3 size);
        Vector3 getColliderBoxSize(size_t index) const;
        // SPHERE, CAPSULE, CYLINDER
        void setColliderRadius(size_t index, float radius);
        float getColliderRadius(size_t index) const;
        // CAPSULE, TAPERED_CAPSULE, CYLINDER
        void setColliderHalfHeight(size_t index, float halfHeight);
        float getColliderHalfHeight(size_t index) const;
        // TAPERED_CAPSULE
        void setColliderTaperedRadii(size_t index, float topRadius, float bottomRadius);
        // CONVEX_HULL (points) or MESH (points and triangles), any size; the source becomes
        // RAW_VERTICES / RAW_MESH
        void setColliderVertices(size_t index, std::vector<Vector3> vertices);
        void setColliderVertices(size_t index, std::vector<Vector3> vertices, std::vector<uint32_t> indices);
        std::vector<Vector3> getColliderVertices(size_t index) const;
        std::vector<uint32_t> getColliderIndices(size_t index) const;
        // Unity isTrigger: overlaps reported, nothing pushed
        void setColliderTrigger(size_t index, bool trigger);
        bool isColliderTrigger(size_t index) const;
        // a .physicsmaterial path ("" = none) or a script's material, over the body's
        void setColliderMaterial(size_t index, const std::string& path);
        void setColliderMaterial(size_t index, const PhysicsMaterial& material);
        void clearColliderMaterial(size_t index);
        // the material the collider uses (its own, else the body's); invalid when none
        PhysicsMaterial getColliderMaterial(size_t index) const;
        std::string getColliderMaterialPath(size_t index) const;
        // Unity collider.material: a collider using an asset gets its own copy of it (the asset
        // and its other users keep theirs); one without a material gets one of its own with the
        // body's surface and the legacy combine rule
        void setColliderFriction(size_t index, float friction);
        void setColliderBounciness(size_t index, float bounciness);
        // what it touches with (its material's, else the body's friction / restitution)
        float getColliderFriction(size_t index) const;
        float getColliderBounciness(size_t index) const;

        void setColliderDensity(float density);
        void setColliderDensity(size_t index, float density);
        float getColliderDensity() const;
        float getColliderDensity(size_t index) const;

        // false (and an error naming the collider) when a static-only collider (mesh,
        // heightfield) is asked to move
        bool setType(BodyType type);
        BodyType getType() const;
        // Unity isKinematic: true = KINEMATIC, false = DYNAMIC
        bool isKinematic() const;
        void setKinematic(bool kinematic);

        void setMotionQuality(RigidbodyMotionQuality motionQuality);
        RigidbodyMotionQuality getMotionQuality() const;

        bool canBeKinematicOrDynamic() const;

        void activate();
        void deactivate();

        bool isSensor() const;
        void setIsSensor(bool sensor);

        bool isCollideKinematicVsNonDynamic() const;
        void setCollideKinematicVsNonDynamic(bool collideKinematicVsNonDynamic);

        void setAllowedDOFsAll();
        void setAllowedDOFs2DPlane();
        void setAllowedDOFs(bool translationX, bool translationY, bool translationZ, bool rotationX, bool rotationY, bool rotationZ);
        // what setAllowedDOFs set (all true by default)
        void getAllowedDOFs(bool& translationX, bool& translationY, bool& translationZ, bool& rotationX, bool& rotationY, bool& rotationZ) const;

        // kg: the explicit mass (automaticMass off), else the colliders' volume x density
        float getMass() const;
        // turns automaticMass off and keeps the mass (saved, applied at once)
        void setMass(float mass);
        bool isAutomaticMass() const;
        void setAutomaticMass(bool automaticMass);
        // body-local, world units; setting it turns automaticCenterOfMass off (Unity)
        Vector3 getCenterOfMass() const;
        void setCenterOfMass(Vector3 centerOfMass);
        bool isAutomaticCenterOfMass() const;
        void setAutomaticCenterOfMass(bool automaticCenterOfMass);
        void resetCenterOfMass();
        // the principal moments (kg m^2) and their frame; setting them turns
        // automaticInertiaTensor off
        Vector3 getInertiaTensor() const;
        void setInertiaTensor(Vector3 inertiaTensor);
        Quaternion getInertiaTensorRotation() const;
        void setInertiaTensorRotation(Quaternion rotation);
        bool isAutomaticInertiaTensor() const;
        void setAutomaticInertiaTensor(bool automaticInertiaTensor);
        void resetInertiaTensor();

        // Unity drag / angularDrag (linearDamping / angularDamping in Unity 6); Jolt's 0.05 default
        float getLinearDamping() const;
        void setLinearDamping(float linearDamping);
        float getAngularDamping() const;
        void setAngularDamping(float angularDamping);
        // m/s and rad/s
        float getMaxLinearVelocity() const;
        void setMaxLinearVelocity(float maxLinearVelocity);
        float getMaxAngularVelocity() const;
        void setMaxAngularVelocity(float maxAngularVelocity);

        RigidbodyInterpolation getInterpolation() const;
        void setInterpolation(RigidbodyInterpolation interpolation);

        // Unity IsSleeping / Sleep / WakeUp
        bool isSleeping() const;
        void sleep();
        void wakeUp();

        // default material of the colliders without one
        void setMaterial(const std::string& path);
        void setMaterial(const PhysicsMaterial& material);
        void clearMaterial();
        PhysicsMaterial getMaterial() const;
        std::string getMaterialPath() const;

        void setOverrideMassAndInertia(Vector3 solidBoxSize, float solidBoxDensity);
        // setOverrideMassAndInertia was called: the mass and inertia come from its solid box
        bool isOverrideMassProperties() const;

        float getGravityFactor() const;
        void setGravityFactor(float gravityFactor);

        void setBitsFilter(uint16_t category, uint16_t mask);

        uint16_t getCategoryBitsFilter() const;
        void setCategoryBitsFilter(uint16_t category);

        uint16_t getMaskBitsFilter() const;
        void setMaskBitsFilter(uint16_t mask);

        uint32_t getCollisionGroupID() const;
        void setCollisionGroupID(uint32_t group);

        bool isAllowSleeping() const;
        void setAllowSleeping(bool allowSleeping);

        float getFriction() const;
        void setFriction(float friction);

        float getRestitution() const;
        void setRestitution(float restitution);

        // Floating in a water (a mesh drawn with library:water, Water.h): the buoyancy factor
        // (1 neutral, more floats, less sinks, 0 ignores the water) and the drags in it,
        // applied at each fixed step to a dynamic body
        float getBuoyancy() const;
        void setBuoyancy(float buoyancy);

        float getWaterDrag() const;
        void setWaterDrag(float waterDrag);

        float getWaterAngularDrag() const;
        void setWaterAngularDrag(float waterAngularDrag);

        // Body pose in world space. The transform only reaches the body between fixed
        // steps, so place a body from onFixedUpdate with these instead of GameObject.
        Vector3 getPosition() const;
        void setPosition(Vector3 position);

        Quaternion getRotation() const;
        void setRotation(Quaternion rotation);

        Vector3 getLinearVelocity() const;
        void setLinearVelocity(Vector3 linearVelocity);
        void setLinearVelocityClamped(Vector3 linearVelocity);

        Vector3 getAngularVelocity() const;
        void setAngularVelocity(Vector3 angularVelocity);
        void setAngularVelocityClamped(Vector3 angularVelocity);

        // center of mass
        Vector3 getPointVelocityCOM(Vector3 pointRelativeToCOM) const;
        Vector3 getPointVelocity(Vector3 point) const;

        Vector3 getAccumulatedForce() const;
        Vector3 getAccumulatedTorque() const;
        Matrix4 getInverseInertia() const;

        // Cleared by each fixed step: apply from onFixedUpdate, unscaled by dt.
        // Single-vector overload is the center force (no applyForceToCenter).
        void applyForce(const Vector3& force);
        void applyForce(const Vector3& force, const Vector3& point);
        void applyTorque(const Vector3& torque);
        void applyImpulse(const Vector3& impulse);
        void applyImpulse(const Vector3& impulse, const Vector3& point);
        void applyAngularImpulse(const Vector3& angularImpulse);
        bool applyBuoyancyImpulse(const Vector3& surfacePosition, const Vector3& surfaceNormal, const float buoyancy, const float linearDrag, const float angularDrag, const Vector3& fluidVelocity, const Vector3& gravity, const float deltaTime);

        Vector3 getCenterOfMassPosition();
    };
}

#endif //RIGIDBODY_H