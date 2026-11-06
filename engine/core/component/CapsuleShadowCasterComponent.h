// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef CAPSULESHADOWCASTER_COMPONENT_H
#define CAPSULESHADOWCASTER_COMPONENT_H

#include "Export.h"
#include "ecs/Entity.h"
#include "math/Vector3.h"

#include <string>
#include <vector>

namespace jasperengine{

    // the bone axis a capsule lies along (Unity's CapsuleCollider Direction)
    enum class CapsuleDirection{
        X_AXIS,
        Y_AXIS,
        Z_AXIS
    };

    // what a Capsule Shadow Caster's meshes cast besides their capsules
    enum class CapsuleCastShadows{
        ON,            // their shadow maps too (the meshes' own Cast Shadows decides)
        CAPSULES_ONLY  // the capsules alone: the meshes leave the shadow maps, so a moving
                       // character keeps the static shadow cache valid
    };

    // One capsule, in the space of a bone (or of the GameObject when bone is empty): a segment of
    // length between the centres of its two caps, along direction through offset, of radius.
    // Sizes scale with the bone's world scale, as a Unity collider's do.
    struct JASPERENGINE_API CapsuleShadowShape{
        std::string bone;
        float radius = 0.1f;
        float length = 0.5f;
        Vector3 offset = Vector3(0.0f, 0.0f, 0.0f);
        CapsuleDirection direction = CapsuleDirection::Y_AXIS;

        bool operator==(const CapsuleShadowShape& o) const{
            return bone == o.bone && radius == o.radius && length == o.length && offset == o.offset && direction == o.direction;
        }
        bool operator!=(const CapsuleShadowShape& o) const{ return !(*this == o); }
    };

    // Analytic soft shadows of a character from up to MAX_CAPSULES capsules on its bones
    // (docs/specs/S57-forge-lighting.md R2, after The Forge's UT28): the main directional light's
    // occlusion of every receiver pixel, without a shadow map. The scene enables them (Scene
    // Settings > Capsule Shadows), and the nearest casters to the camera are drawn.
    struct JASPERENGINE_API CapsuleShadowCasterComponent{
        static constexpr int MAX_CAPSULES = 8;

        bool enabled = true;
        CapsuleCastShadows castShadows = CapsuleCastShadows::ON;
        std::vector<CapsuleShadowShape> capsules;

        // runtime: the bone entity of each capsule (resolved by name under the GameObject), and
        // what they were resolved for
        std::vector<Entity> boneEntities;
        uint64_t resolvedHash = 0;
    };

}

#endif //CAPSULESHADOWCASTER_COMPONENT_H
