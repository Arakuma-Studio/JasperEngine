// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef RAGDOLL_H
#define RAGDOLL_H

#ifndef JASPERENGINE_PHYSICS_3D
#error "Ragdoll needs 3D Physics, enabled in Project Settings > Build"
#endif

#include "EntityHandle.h"
#include "component/RagdollComponent.h"
#include "math/Vector3.h"
#include "util/FunctionSubscribe.h"

#include <vector>

namespace jasperengine{

    class Rigidbody;

    // A character's ragdoll (RagdollComponent): animated (kinematic parts following the animated
    // bones) or simulated (dynamic parts moving the bones and the skin). A change of `simulated`
    // reaches the parts and the Animation Player at the next fixed step.
    class JASPERENGINE_API Ragdoll: public EntityHandle{
    private:
        RagdollComponent& ragdoll() const;

    public:
        Ragdoll(Scene* scene, Entity entity);
        virtual ~Ragdoll();

        Ragdoll(const Ragdoll& rhs);
        Ragdoll& operator=(const Ragdoll& rhs);
        Ragdoll(Ragdoll&& rhs) noexcept;
        Ragdoll& operator=(Ragdoll&& rhs) noexcept;

        // component checkbox (Unity Behaviour.enabled): a disabled Ragdoll leaves its parts as they are
        void setEnabled(bool enabled);
        bool isEnabled() const;

        bool isSimulated() const;
        void setSimulated(bool simulated);

        Entity getRoot() const;
        void setRoot(Entity root);
        Entity getAnimator() const;
        void setAnimator(Entity animator);
        bool isInheritVelocity() const;
        void setInheritVelocity(bool inheritVelocity);
        bool isSelfCollision() const;
        void setSelfCollision(bool selfCollision);
        float getTotalMass() const;
        void setTotalMass(float totalMass);

        // the Rigidbodies below root (root excluded)
        size_t getPartCount() const;
        std::vector<Rigidbody> getParts() const;
        std::vector<Entity> getPartEntities() const;

        // Unity's "hit bone" idiom: the impulse (N s) at a world point, given to the part whose
        // centre of mass is nearest it
        void applyImpulse(const Vector3& impulse, const Vector3& point);
        // the force (N) over every part by its mass (each part accelerates alike)
        void applyForce(const Vector3& force);
        // Called (with the ragdoll's entity) for every ragdoll of the scene just before it goes
        // back to animated, while its parts still hold the simulated pose (a blend hook)
        FunctionSubscribe<void(int)>& onBeforeAnimatedRestore() const;
    };
}

#endif //RAGDOLL_H
