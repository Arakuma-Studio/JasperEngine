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

#include "Jolt/Jolt.h"
#include "Jolt/Physics/Body/Body.h"
#include "Jolt/Physics/Body/BodyInterface.h"

namespace jasperengine{

    class GameObject;

    class JASPERENGINE_API Rigidbody: public EntityHandle{
    protected:
        void checkBody(const RigidbodyComponent& body) const;

        void applyAllowedDOFs(JPH::EAllowedDOFs allowedDOFs);

        const JPH::BodyLockInterface& getBodyLockInterface() const;
        JPH::BodyInterface& getBodyInterface() const;

    public:

        // component checkbox (Unity Behaviour.enabled): a disabled Rigidbody is skipped by its system
        void setEnabled(bool enabled);
        bool isEnabled() const;
        Rigidbody(Scene* scene, Entity entity);
        virtual ~Rigidbody();

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
        int createMeshCollider(std::vector<Vector3> vertices, std::vector<uint16_t> indices);
        int createMeshCollider(Vector3 position, Quaternion rotation, std::vector<Vector3> vertices, std::vector<uint16_t> indices);
        int createHeightFieldCollider();
        int createHeightFieldCollider(unsigned int samplesSize);

        size_t getNumColliders() const;
        Collider3DType getColliderType(size_t index) const;

        void setColliderDensity(float density);
        void setColliderDensity(size_t index, float density);
        float getColliderDensity() const;
        float getColliderDensity(size_t index) const;

        void setType(BodyType type);
        BodyType getType() const;

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

        float getMass() const;
        // Live body only, lost on reload; persist mass via Collider3D::density.
        void setMass(float mass);

        void setOverrideMassAndInertia(Vector3 solidBoxSize, float solidBoxDensity);

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