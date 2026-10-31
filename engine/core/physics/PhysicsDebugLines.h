// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (physics-3d 6.1.12)
// - Purpose: the wireframes of 3D colliders and joints, one generator for the editor's Scene view
//   gizmos (SceneRender3D) and the runtime debug draw (Physics.debugDraw, PhysicsSystem::draw).
// - Colliders: box, sphere, capsule, tapered capsule, cylinder, convex hull (raw points or the
//   source mesh's hull), mesh edges and the heightfield's bounds, sized by the body's world scale
//   exactly as JoltShapeBuilder sizes the Jolt shapes. Source-mesh hulls and meshes walk their whole
//   vertex buffer: their edges are cached in an EdgeCache per collider until what feeds them changes.
// - Joints: the anchors (WORLD points or LOCAL points of their bodies), axes, hinge limit arcs,
//   slider limit bars, cone and swing-twist cones.
// - Lines are emitted, not stored: the caller draws them (LineRenderer) in its own colour.
// - Main thread; no Jolt access (the poses are given).

#ifndef PHYSICS_DEBUG_LINES_H
#define PHYSICS_DEBUG_LINES_H

#include "Export.h"
#include "ecs/Entity.h"
#include "math/Matrix4.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"

#include <cstdint>
#include <functional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace jasperengine{

    class Scene;
    struct Collider3D;
    struct RigidbodyComponent;
    struct Joint3DComponent;
    struct Transform;

    class JASPERENGINE_API PhysicsDebugLines{
    public:
        using Emit = std::function<void(const Vector3& from, const Vector3& to)>;

        // Mesh and hull shapes walk their whole source buffer to build these, so they are kept
        // until the signature of what feeds them changes
        struct EdgeCache{
            bool valid = false;
            uint64_t signature = 0;
            std::vector<std::pair<Vector3, Vector3>> edges; // body-local space
        };

        // A body pose: world position, rotation and the world scale its colliders are sized by
        struct Pose{
            Vector3 position = Vector3::ZERO;
            Quaternion rotation = Quaternion::IDENTITY;
            Vector3 scale = Vector3::UNIT_SCALE;
        };

        // Every collider of the body in world space at `pose`; transform is the body's Transform
        // (source meshes are mapped through it). emit gets each line and its collider's index.
        static void bodyEdges(Scene* scene, Entity entity, const Transform& transform, const RigidbodyComponent& body, const Pose& pose,
                              std::vector<EdgeCache>& caches, const std::function<void(const Vector3&, const Vector3&, size_t)>& emit);

        // The joint's lines in world space, the bodies at the poses poseOf gives (false: no body).
        // kind: 0 the joint (anchor to anchor), 1 helpers (anchor marks), 2 axes, 3 limits.
        static void jointEdges(Scene* scene, const Joint3DComponent& joint, const std::function<bool(Entity, Pose&)>& poseOf,
                               const std::function<void(const Vector3&, const Vector3&, int)>& emit);
        // The anchors of the joint in world space (LOCAL: through the bodies' poses)
        static void jointAnchors(const Joint3DComponent& joint, const std::function<bool(Entity, Pose&)>& poseOf,
                                 Vector3& anchorA, Vector3& anchorB);

        // A unit centre-of-mass mark (three short axes) at a world point
        static void crossMark(const Vector3& center, float size, const Emit& emit);

        // Convex hull edges of a point set, each unique edge once (QuickHull; 2D monotone chain
        // when coplanar); a large cloud is first reduced to its extremes
        static void buildConvexHullEdges(std::vector<Vector3> points, const Emit& emit);
        static uint64_t quantizeHullKey(const Vector3& p);
        static void pushUniqueHullPoint(std::vector<Vector3>& points, std::unordered_set<uint64_t>& seen, const Vector3& p);
        static void reduceToExtremes(std::vector<Vector3>& points);

        static constexpr float kHullQuantizeScale = 4096.0f;
        // Above this the cloud is reduced to its extremes first. Matches
        // ConvexHullShape::cMaxPointsInHull, the cap Jolt applies to the same shape.
        static constexpr size_t kMaxHullInputPoints = 256;
        static constexpr size_t kMaxSourceParentHops = 64;
    };

}

#endif //PHYSICS_DEBUG_LINES_H
