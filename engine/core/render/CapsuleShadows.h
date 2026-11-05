// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef CAPSULESHADOWS_H
#define CAPSULESHADOWS_H

#include "Export.h"
#include "component/CapsuleShadowCasterComponent.h"
#include "math/AABB.h"
#include "math/Matrix4.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"
#include "math/Vector4.h"

#include <string>
#include <vector>

namespace jasperengine {

    // Capsule shadows (docs/specs/S57-forge-lighting.md R2): the geometry the renderer and the
    // editor share. The occlusion is the closest approach of the ray from a receiver towards the
    // light to each capsule's segment, smoothstepped over a penumbra that widens with the
    // distance along the ray (includes/capsule_shadows.glsl evaluates it per pixel; occlusion()
    // here is its CPU twin, for tests). Re-derived from the geometry, after The Forge's UT28.
    class JASPERENGINE_API CapsuleShadows {
    public:
        // the casters drawn at once (the nearest to the camera) and their capsules
        static constexpr int MAX_CASTERS = 4;
        static constexpr int MAX_CAPSULES = MAX_CASTERS * CapsuleShadowCasterComponent::MAX_CAPSULES;

        // a capsule in world space: the segment a..b and its radius
        struct World {
            Vector3 a;
            Vector3 b;
            float radius = 0.0f;
        };

        // a bone of a skeleton in world space, for the fit (parent: index into the list, -1 for a root)
        struct Bone {
            std::string name;
            int parent = -1;
            Vector3 position;
            Quaternion rotation;
            Vector3 scale = Vector3(1.0f, 1.0f, 1.0f);
        };

        // the world capsule of a shape on a bone (or the GameObject) whose model matrix is given
        static World toWorld(const CapsuleShadowShape& shape, const Matrix4& boneModel);
        // the shape on a bone of the world capsule a..b of radius (fit and the gizmo's handles)
        static CapsuleShadowShape fromWorld(const std::string& boneName, const Bone& bone, const Vector3& a, const Vector3& b, float radius);

        // Fit To Skeleton: the humanoid slots of the bones (pose::autoDetectHumanoid) give eight
        // capsules, as The Forge fits its character: the head (neck to the head's end), the spine
        // (hips to the upper chest), each upper leg, lower leg and foot. Radii follow the body's
        // height. The capsules of the slots not found are left out; false when the bones are not
        // a humanoid (no hips or no legs), with the reason in error.
        static bool fitToSkeleton(const std::vector<Bone>& bones, std::vector<CapsuleShadowShape>& capsules, std::string* error = nullptr);

        // how far along the light a capsule can shadow: where its penumbra has grown past twice
        // its radius (softness: the penumbra's widening per metre along the ray)
        static float reach(const World& capsule, float softness);
        // whether a capsule can shadow anything in a receiver's bounds, for a light from towardsLight
        static bool reaches(const World& capsule, const Vector3& towardsLight, float softness, const AABB& receiver);

        // the visibility (1 lit .. 0 occluded) of point towards a light (unit towardsLight) past
        // one capsule, as includes/capsule_shadows.glsl computes it
        static float occlusion(const World& capsule, const Vector3& point, const Vector3& towardsLight, float softness);
    };

}

#endif // CAPSULESHADOWS_H
