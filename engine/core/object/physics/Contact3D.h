// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef Contact3D_H
#define Contact3D_H

#include "Entity.h"
#include "Rigidbody.h"
#include "math/Vector3.h"

#include <vector>

#include "Jolt/Physics/Body/Body.h"
#include "Jolt/Physics/Collision/ContactListener.h"

#include <cstdint>
#include <memory>

namespace jasperengine{

    // Where two bodies touch: the bodies and their sub shapes (Jolt's ids)
    struct JASPERENGINE_API ContactPairKey3D {
        uint32_t body1 = 0;
        uint32_t body2 = 0;
        uint32_t subShape1 = 0;
        uint32_t subShape2 = 0;

        bool operator<(const ContactPairKey3D& other) const;
        bool operator==(const ContactPairKey3D& other) const;
    };

    // What a contact manifold had at the step, for a Contact3D delivered after it
    struct JASPERENGINE_API Contact3DSnapshot{
        ContactPairKey3D key;
        Vector3 baseOffset;
        Vector3 worldSpaceNormal;
        float penetrationDepth = 0.0f;
        size_t colliderIndex1 = 0;
        size_t colliderIndex2 = 0;
        // relative to baseOffset, at most 4 per side
        std::vector<Vector3> pointsOn1;
        std::vector<Vector3> pointsOn2;
        float combinedFriction = 0.0f;
        float combinedRestitution = 0.0f;
        bool sensor = false;
        float approachSpeed = 0.0f;
    };

    // A 3D contact manifold of the global events (onContactAdded3D...). Delivered after the
    // step (the default, on the main thread) it is a snapshot: a setting changed on it
    // (friction, restitution, sensor) applies to that contact from the next step, for as long
    // as the two bodies keep touching there. Live (setters apply at once) inside the step:
    // PhysicsSystem synchronousContactEvents3D, and the C++ onContactModify3D.
    class JASPERENGINE_API Contact3D{
    private:
        Scene* scene = nullptr;
        ContactPairKey3D key;
        const JPH::Body* body1 = nullptr;
        const JPH::Body* body2 = nullptr;
        const JPH::ContactManifold* contactManifold = nullptr;
        JPH::ContactSettings* contactSettings = nullptr;
        std::shared_ptr<const Contact3DSnapshot> snapshot;

        // a snapshot's own copy changed (the next step gets it from PhysicsSystem)
        Contact3DSnapshot& editSnapshot();

    public:
        Contact3D();
        // Jolt's contact, inside the step (on its job threads unless the step is synchronous)
        Contact3D(Scene* scene, const JPH::Body* body1, const JPH::Body* body2, const JPH::ContactManifold* contactManifold, JPH::ContactSettings* contactSettings);
        Contact3D(Scene* scene, std::shared_ptr<const Contact3DSnapshot> snapshot);

        // the values now, for keeping after the call (also of a live one)
        Contact3DSnapshot takeSnapshot() const;
        bool isSnapshot() const { return snapshot != nullptr; }
        virtual ~Contact3D();

        Contact3D(const Contact3D& rhs);
        Contact3D& operator=(const Contact3D& rhs);

        const ContactPairKey3D& getKey() const;
        // null for a snapshot
        const JPH::ContactManifold* getJoltContactManifold() const;
        JPH::ContactSettings* getJoltContactSettings() const;
        // the points of getRelativeContactPointsOnA (getContactPointCount)
        size_t getNumContactPoints() const;

        // ContactManifold
        Vector3 getBaseOffset() const;
        Vector3 getWorldSpaceNormal() const;
        float getPenetrationDepth() const;
        size_t getColliderIndex1() const;
        size_t getColliderIndex2() const;
        size_t getContactPointCount() const;
        // Relative to the base offset; a zero vector past the last point
        Vector3 getRelativeContactPointsOnA(size_t index) const;
        Vector3 getRelativeContactPointsOnB(size_t index) const;

        // ContactSettings (on a snapshot a change applies from the next step: see above)
        float getCombinedFriction() const;
        void setCombinedFriction(float combinedFriction);

        float getCombinedRestitution() const;
        void setCombinedRestitution(float combinedRestitution);

        bool isSensor() const;
        void setIsSensor(bool sensor);

        // How fast the bodies came together along the normal before the solver, units a second:
        // max(0, (v1 - v2) . n), at the manifold's centre
        float getApproachSpeed() const;
    };
}

#endif //Contact3D_H
