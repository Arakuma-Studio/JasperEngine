// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Culling Groups (docs/specs/culling-group-and-simulation-distance.md R1-R9), Unity's
// CullingGroup: bounding spheres, fixed or following GameObjects, sorted into distance bands
// from a reference point and tested against the frustum of one camera or of any enabled one,
// once a frame while the scene runs (CullingSystem). onStateChanged is told each sphere whose
// visibility or band changed. Frustum only: there is no occlusion culling.
//
// A CullingGroup is a handle: copies share the group. dispose() ends it; a group made inside a Lua
// script's callback is disposed when that script instance is released, any other with its scene.
// Sphere indices are 0-based here (1-based in Lua); bands are 0-based (0 nearest), -1 unknown.
// Main thread only.

#ifndef CULLING_GROUP_H
#define CULLING_GROUP_H

#include "Export.h"
#include "ecs/Entity.h"
#include "math/Vector3.h"
#include "util/FunctionSubscribe.h"

#include <memory>
#include <cstddef>
#include <cstdint>
#include <span>

namespace jasperengine{

    class Scene;

    struct JASPERENGINE_API BoundingSphere{
        Vector3 position;
        float radius = 0.0f;

        BoundingSphere() = default;
        BoundingSphere(const Vector3& position, float radius): position(position), radius(radius) {}
    };

    struct JASPERENGINE_API CullingGroupEvent{
        uint32_t index = 0;
        bool isVisible = false;
        bool wasVisible = false;
        // bands: 0 nearest, -1 unknown (before the first evaluation)
        int8_t currentDistance = -1;
        int8_t previousDistance = -1;

        bool hasBecomeVisible() const { return isVisible && !wasVisible; }
        bool hasBecomeInvisible() const { return !isVisible && wasVisible; }
    };

    class JASPERENGINE_API CullingGroup{
    public:
        static constexpr uint32_t MAX_SPHERES = 100000;
        static constexpr size_t MAX_DISTANCES = 8;
        static constexpr uint32_t MAX_GROUPS = 256;

        // an invalid handle
        CullingGroup() = default;
        // create(scene)
        explicit CullingGroup(Scene* scene);

        // a new group of the scene; an invalid handle past MAX_GROUPS (an error naming the limit)
        static CullingGroup create(Scene* scene);
        void dispose();
        bool isValid() const;

        // false: not evaluated, no events; the states freeze until enabled again
        void setEnabled(bool enabled);
        bool isEnabled() const;

        // NULL_ENTITY: visible in any enabled camera of the scene
        void setTargetCamera(Entity camera);
        Entity getTargetCamera() const;

        void setBoundingSpheres(std::span<const BoundingSphere> spheres);
        // the first `count`: shrinking forgets the rest, growing adds spheres at the origin, radius 0
        void setBoundingSphereCount(uint32_t count);
        uint32_t getBoundingSphereCount() const;
        void setSphere(uint32_t index, const BoundingSphere& sphere);
        BoundingSphere getSphere(uint32_t index) const;
        // the sphere follows the GameObject's world position plus offset at each evaluation
        void setSphereTarget(uint32_t index, Entity entity, float radius, const Vector3& offset = Vector3(0, 0, 0));
        // the last sphere and its state move into `index`
        void eraseSwapBack(uint32_t index);

        // 0 to MAX_DISTANCES ascending, finite, positive distances: band b holds [d[b-1], d[b])
        void setBoundingDistances(std::span<const float> distances);
        void setDistanceReferencePoint(const Vector3& point);
        // followed at each evaluation
        void setDistanceReferencePoint(Entity entity);
        // metres a sphere must pass a band's edge by before it moves to a farther band (default 0)
        void setBandHysteresis(float metres);
        float getBandHysteresis() const;

        bool isVisible(uint32_t index) const;
        // the band; -1 before the first evaluation
        int getDistance(uint32_t index) const;
        // the indices of the matching spheres written into `out`, skipping the first `first`
        // matches: the count written
        uint32_t queryIndices(bool visible, std::span<uint32_t> out, uint32_t first = 0) const;
        uint32_t queryIndices(int band, std::span<uint32_t> out, uint32_t first = 0) const;
        uint32_t queryIndices(bool visible, int band, std::span<uint32_t> out, uint32_t first = 0) const;

        // told each changed sphere after the whole group was evaluated, in ascending index order;
        // the first evaluation reports every sphere (previous band -1)
        FunctionSubscribe<void(CullingGroupEvent)>& onStateChanged();

        // nullptr once the scene is gone (a handle a persistent scene's script kept)
        Scene* getScene() const { return sceneLifetime.expired() ? nullptr : scene; }
        uint32_t getSlot() const { return slot; }
        uint32_t getGeneration() const { return generation; }

        bool operator==(const CullingGroup& other) const {
            return scene == other.scene && slot == other.slot && generation == other.generation;
        }

    private:
        friend class CullingSystem;
        CullingGroup(Scene* scene, uint32_t slot, uint32_t generation);

        Scene* scene = nullptr;
        // the scene's lifetime (Scene::getLifetime), as EntityHandle watches it
        std::weak_ptr<char> sceneLifetime;
        uint32_t slot = 0;
        uint32_t generation = 0;
    };

}

#endif //CULLING_GROUP_H
