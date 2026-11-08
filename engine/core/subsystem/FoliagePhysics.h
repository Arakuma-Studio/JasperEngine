// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FOLIAGE_PHYSICS_H
#define FOLIAGE_PHYSICS_H

// Trees' capsules (T5 R18, R33): while the scene simulates, one static Jolt body per cell with trees
// of a prototype that collides, on an engine-owned entity. colliderIndex in contacts and rays is the
// tree's place in the cell's id list, so a hit names the tree.

#include "ecs/Entity.h"
#include "math/Vector3.h"
#include "terrain/FoliageTypes.h"
#include "util/Signal.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace jasperengine {

    class Scene;

    namespace foliagephysics {

        // One tree's capsule in terrain-local space, from its base
        struct TreeCapsule {
            uint32_t id = 0;
            Vector3 base;
            float radius = 0.5f;
            float height = 4.0f;
        };

        // What a cell keeps of its body
        struct CellBody {
            Entity entity = NULL_ENTITY;
            std::vector<uint32_t> ids;  // the tree of a collider index
        };

        // The scene steps a 3D world now: bodies are worth making
        bool simulating(Scene& scene);
        // Makes, replaces or (no capsules) removes the cell's body. Compound built here, in the frame.
        void setCellBody(Scene& scene, Entity terrain, CellBody& body, const std::vector<TreeCapsule>& capsules);
        void destroyCellBody(Scene& scene, CellBody& body);

        // Started tree contacts, on the main thread after the step (R33): the tree's body, its collider, the other body
        using ContactFn = std::function<void(Entity treeBody, size_t collider, Entity other, const Vector3& point, const Vector3& normal, float speed)>;
        [[nodiscard]] Subscription subscribeContacts(Scene& scene, ContactFn handler);

        // S19 R10's updateTiles for this neighbourhood on each surface (R34). Without a navigation
        // system or surface it does nothing.
        void updateNavigationAround(Scene& scene, const Vector3& position, float radius);

    }

}

#endif // FOLIAGE_PHYSICS_H
