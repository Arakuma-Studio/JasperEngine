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

#include <memory>

namespace jasperengine{

    // What a contact manifold had at the step, for a Contact3D delivered after it
    struct JASPERENGINE_API Contact3DSnapshot{
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
    };

    // A 3D contact manifold of the global events (onContactAdded3D...). Delivered after the
    // step (the default) it is a snapshot: the getters work, the setters warn once per scene
    // and do nothing. Live (setters work) inside the step: PhysicsSystem
    // synchronousContactEvents3D, and the C++ onContactModify3D.
    class JASPERENGINE_API Contact3D{
    private:
        Scene* scene;
        const JPH::Body* body1;
        const JPH::Body* body2;
        const JPH::ContactManifold* contactManifold;
        JPH::ContactSettings* contactSettings;
        std::shared_ptr<const Contact3DSnapshot> snapshot;

        void warnSnapshotSetter() const;

    public:
        Contact3D(Scene* scene, const JPH::Body* body1, const JPH::Body* body2, const JPH::ContactManifold* contactManifold, JPH::ContactSettings* contactSettings);
        Contact3D(Scene* scene, std::shared_ptr<const Contact3DSnapshot> snapshot);

        // the values now, for keeping after the call (also of a live one)
        Contact3DSnapshot takeSnapshot() const;
        bool isSnapshot() const { return snapshot != nullptr; }
        virtual ~Contact3D();

        Contact3D(const Contact3D& rhs);
        Contact3D& operator=(const Contact3D& rhs);

        // null for a snapshot
        const JPH::ContactManifold* getJoltContactManifold() const;
        JPH::ContactSettings* getJoltContactSettings() const;
        size_t getNumContactPoints() const;

        // ContactManifold
        Vector3 getBaseOffset() const;
        Vector3 getWorldSpaceNormal() const;
        float getPenetrationDepth() const;
        size_t getColliderIndex1() const;
        size_t getColliderIndex2() const;
        Vector3 getRelativeContactPointsOnA(size_t index) const;
        Vector3 getRelativeContactPointsOnB(size_t index) const;


        // ContactSettings
        float getCombinedFriction() const;
        void setCombinedFriction(float combinedFriction);

        float getCombinedRestitution() const;
        void setCombinedRestitution(float combinedRestitution);

        bool isSensor() const;
        void setIsSensor(bool sensor);
    };
}

#endif //Contact3D_H