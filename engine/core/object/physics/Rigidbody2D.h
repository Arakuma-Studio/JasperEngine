// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef RIGIDBODY2D_H
#define RIGIDBODY2D_H

#ifndef JASPERENGINE_PHYSICS_2D
#error "Rigidbody2D needs 2D Physics, enabled in Project Settings > Build"
#endif

#include "EntityHandle.h"
#include "math/Vector2.h"
#include "component/Rigidbody2DComponent.h"
#include "physics/PhysicsMaterial.h"


namespace jasperengine{

    class Contact2D;
    class GameObject;

    class JASPERENGINE_API Rigidbody2D: public EntityHandle{
    protected:
        void checkBody(const Rigidbody2DComponent& body) const;
        // the collider at index; out of range: an error (a Lua error)
        Collider2D& colliderAt(size_t index) const;
        // a collider's data changed: its shapes are made again at the next step
        void colliderChanged(size_t index);
        // the explicit mass fields changed
        void massChanged();

    public:
        Rigidbody2D(Scene* scene, Entity entity);
        virtual ~Rigidbody2D();

        Rigidbody2D(const Rigidbody2D& rhs);
        Rigidbody2D& operator=(const Rigidbody2D& rhs);
        Rigidbody2D(Rigidbody2D&& rhs) noexcept;
        Rigidbody2D& operator=(Rigidbody2D&& rhs) noexcept;

        b2BodyId getBox2DBody() const;
        // the first Box2D shape of the collider (a polygon split in convex pieces has several)
        b2ShapeId getBox2DShape(size_t index) const;
        std::vector<b2ShapeId> getBox2DShapes(size_t index) const;
        b2ChainId getBox2DChain(size_t index) const;

        GameObject getAttachedObject();

        float getPointsToMeterScale() const;

        void load();

        int createBoxCollider(float width, float height);
        int createCenteredBoxCollider(float width, float height);
        int createCenteredBoxCollider(float width, float height, Vector2 center, float angle);
        int createRoundedBoxCollider(float width, float height, float radius);
        // any simple polygon of 3+ points, concave too (split in convex pieces)
        int createPolygonCollider(std::vector<Vector2> vertices);
        int createCircleCollider(Vector2 center, float radius);
        int createCapsuleCollider(Vector2 center1, Vector2 center2, float radius);
        int createSegmentCollider(Vector2 point1, Vector2 point2);
        // one-sided: counter-clockwise collides outside, clockwise inside. 2 or 3 points (not a
        // loop of 4+) make two-sided segments (Unity EdgeCollider2D)
        int createChainCollider(std::vector<Vector2> vertices, bool loop);

        void removeAllColliders();
        // the later colliders shift down an index; false when out of range
        bool removeCollider(size_t index);

        // collider edits: only that collider is made again, the body keeps its velocity,
        // joints and the contacts of its other colliders
        void setColliderPoints(size_t index, std::vector<Vector2> points);
        std::vector<Vector2> getColliderPoints(size_t index) const;
        void setColliderLoop(size_t index, bool loop);
        bool isColliderLoop(size_t index) const;
        void setColliderRadius(size_t index, float radius);
        float getColliderRadius(size_t index) const;
        void setColliderCenter(size_t index, Vector2 center);
        Vector2 getColliderCenter(size_t index) const;
        void setColliderSegment(size_t index, Vector2 pointA, Vector2 pointB);
        // a .physicsmaterial path ("" = none) or a script's material, over the body's
        void setColliderMaterial(size_t index, const std::string& path);
        void setColliderMaterial(size_t index, const PhysicsMaterial& material);
        void clearColliderMaterial(size_t index);
        // the material the collider uses (its own, else the body's); invalid when none
        PhysicsMaterial getColliderMaterial(size_t index) const;
        std::string getColliderMaterialPath(size_t index) const;
        // Unity isTrigger: the sensor flag
        void setColliderTrigger(size_t index, bool trigger);
        bool isColliderTrigger(size_t index) const;
        // the Box2D shapes of the collider (convex pieces of a polygon, segments of a chain)
        size_t getColliderPieceCount(size_t index) const;

        std::vector<Contact2D> getBodyContacts();
        std::vector<Contact2D> getColliderContacts(size_t index);

        size_t getNumColliders() const;
        Collider2DType getColliderType(size_t index) const;

        void setColliderDensity(float density);
        void setColliderFriction(float friction);
        void setColliderRestitution(float restitution);

        void setColliderDensity(size_t index, float density);
        void setColliderFriction(size_t index, float friction);
        void setColliderRestitution(size_t index, float restitution);

        // no live toggle in Box2D, shapes are rebuilt on the next step
        void setColliderSensor(bool sensor);
        void setColliderSensor(size_t index, bool sensor);

        bool isColliderSensor() const;
        bool isColliderSensor(size_t index) const;

        void setColliderEnableHitEvents(bool hitEvents);
        void setColliderContactEvents(bool contactEvents);
        void setColliderPreSolveEvents(bool preSolveEvent);
        void setColliderSensorEvents(bool sensorEvents);

        void setColliderEnableHitEvents(size_t index, bool hitEvents);
        void setColliderContactEvents(size_t index, bool contactEvents);
        void setColliderPreSolveEvents(size_t index, bool preSolveEvent);
        void setColliderSensorEvents(size_t index, bool sensorEvents);

        float getColliderDensity() const;
        float getColliderFriction() const;
        float getColliderRestitution() const;

        float getColliderDensity(size_t index) const;
        float getColliderFriction(size_t index) const;
        float getColliderRestitution(size_t index) const;

        bool isColliderEnableHitEvents() const;
        bool isColliderContactEvents() const;
        bool isColliderPreSolveEvents() const;
        bool isColliderSensorEvents() const;

        bool isColliderEnableHitEvents(size_t index) const;
        bool isColliderContactEvents(size_t index) const;
        bool isColliderPreSolveEvents(size_t index) const;
        bool isColliderSensorEvents(size_t index) const;

        // Body pose in world space. The transform only reaches the body between fixed
        // steps, so place a body from onFixedUpdate with these instead of GameObject.
        void setPosition(Vector2 position);
        void setAngle(float angle);
        void setLinearVelocity(Vector2 linearVelocity);
        void setAngularVelocity(float angularVelocity);
        void setLinearDamping(float linearDamping);
        void setAngularDamping(float angularDamping);
        void setEnableSleep(bool enableSleep);
        void setAwake(bool awake);
        void setFixedRotation(bool fixedRotation);
        void setBullet(bool bullet);
        void setType(BodyType type);
        void setEnabled(bool enabled);
        void setGravityScale(float gravityScale);

        Vector2 getPosition() const;
        float getAngle() const;
        Vector2 getLinearVelocity() const;
        float getAngularVelocity() const;
        float getLinearDamping() const;
        float getAngularDamping() const;
        bool isEnableSleep() const;
        bool isAwake() const;
        bool isFixedRotation() const;
        bool isBullet() const;
        BodyType getType() const;
        bool isEnabled() const;
        float getGravityScale() const;

        void setBitsFilter(uint16_t categoryBits, uint16_t maskBits);
        void setBitsFilter(size_t colliderIndex, uint16_t categoryBits, uint16_t maskBits);

        void setCategoryBitsFilter(uint16_t categoryBits);
        void setMaskBitsFilter(uint16_t maskBits);
        void setGroupIndexFilter(int16_t groupIndex);

        void setCategoryBitsFilter(size_t colliderIndex, uint16_t categoryBits);
        void setMaskBitsFilter(size_t colliderIndex, uint16_t maskBits);
        void setGroupIndexFilter(size_t colliderIndex, int16_t groupIndex);

        uint16_t getCategoryBitsFilter() const;
        uint16_t getMaskBitsFilter() const;
        int16_t getGroupIndexFilter() const;

        uint16_t getCategoryBitsFilter(size_t colliderIndex) const;
        uint16_t getMaskBitsFilter(size_t colliderIndex) const;
        int16_t getGroupIndexFilter(size_t colliderIndex) const;

        float getMass() const;
        float getRotationalInertia() const;

        // Unity Rigidbody2D mass fields: setting mass, centerOfMass or inertia turns its
        // automatic computation off (useAutoMass, autoCenterOfMass, autoInertia)
        void setMass(float mass);
        bool isUseAutoMass() const;
        void setUseAutoMass(bool useAutoMass);
        // body-local, in collider units (points)
        Vector2 getCenterOfMass() const;
        void setCenterOfMass(Vector2 center);
        bool isAutoCenterOfMass() const;
        void setAutoCenterOfMass(bool autoCenterOfMass);
        // kg m^2 about the centre of mass
        float getInertia() const;
        void setInertia(float inertia);
        bool isAutoInertia() const;
        void setAutoInertia(bool autoInertia);

        CollisionDetectionMode2D getCollisionDetection() const;
        void setCollisionDetection(CollisionDetectionMode2D mode);
        RigidbodySleepMode2D getSleepingMode() const;
        void setSleepingMode(RigidbodySleepMode2D mode);

        // default material of the colliders without one
        void setMaterial(const std::string& path);
        void setMaterial(const PhysicsMaterial& material);
        void clearMaterial();
        PhysicsMaterial getMaterial() const;
        std::string getMaterialPath() const;

        // the mass from the colliders again (then the explicit mass fields, if any)
        void applyMassFromColliders();

        // Cleared by each fixed step: apply from onFixedUpdate, unscaled by dt.
        // Center force needs applyForceToCenter (Rigidbody is the reverse).
        void applyForce(const Vector2& force, const Vector2& point, bool wake);
        void applyForceToCenter(const Vector2& force, bool wake);
        void applyTorque(float torque, bool wake);
        void applyLinearImpulse(const Vector2& impulse, const Vector2& point, bool wake);
        void applyLinearImpulseToCenter(const Vector2& impulse, bool wake);
        void applyAngularImpulse(float impulse, bool wake);

        // S96 WP-E: not capped by the scene's Max Rotation Per Step (wheels), live
        void setAllowFastRotation(bool allowFastRotation);
        bool isAllowFastRotation() const;
    };
}

#endif //RIGIDBODY2D_H