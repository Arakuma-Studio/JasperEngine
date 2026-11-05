// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef CAPSULESHADOWCASTER_H
#define CAPSULESHADOWCASTER_H

#include "GameObject.h"
#include "component/CapsuleShadowCasterComponent.h"

#include <string>

namespace jasperengine{

    // A character's capsule shadows (spec S57 R2): up to 8 capsules on its bones, drawn as the
    // main directional light's soft occlusion while the scene enables capsule shadows
    class JASPERENGINE_API CapsuleShadowCaster: public GameObject{
    public:
        CapsuleShadowCaster(Scene* scene);
        CapsuleShadowCaster(Scene* scene, Entity entity);
        virtual ~CapsuleShadowCaster();

        CapsuleShadowCaster(const CapsuleShadowCaster& rhs);
        CapsuleShadowCaster& operator=(const CapsuleShadowCaster& rhs);
        CapsuleShadowCaster(CapsuleShadowCaster&& rhs) noexcept;
        CapsuleShadowCaster& operator=(CapsuleShadowCaster&& rhs) noexcept;

        void setEnabled(bool enabled);
        bool isEnabled() const;
        // On, or Capsules Only: its meshes then cast no shadow map
        void setCastShadows(CapsuleCastShadows castShadows);
        CapsuleCastShadows getCastShadows() const;

        // a capsule on a bone (a GameObject's name under this one; "" this GameObject), along
        // the bone's local Y; its index, -1 past the eight
        int addCapsule(const std::string& bone, float radius, float length);
        void removeCapsule(int index);
        void clearCapsules();
        int getCapsuleCount() const;

        std::string getCapsuleBone(int index) const;
        void setCapsuleBone(int index, const std::string& bone);
        float getCapsuleRadius(int index) const;
        void setCapsuleRadius(int index, float radius);
        float getCapsuleLength(int index) const;
        void setCapsuleLength(int index, float length);
        Vector3 getCapsuleOffset(int index) const;
        void setCapsuleOffset(int index, Vector3 offset);
        CapsuleDirection getCapsuleDirection(int index) const;
        void setCapsuleDirection(int index, CapsuleDirection direction);

        // Fit To Skeleton: eight capsules on the humanoid bones of this GameObject and its
        // children (the head, the spine, each leg's three); false (the capsules untouched) when
        // they are no humanoid
        bool fitToSkeleton();
    };

}

#endif //CAPSULESHADOWCASTER_H
