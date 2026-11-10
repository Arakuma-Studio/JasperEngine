// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef CONTACT_TRACKER_H
#define CONTACT_TRACKER_H

// Unity-style collision and trigger callbacks (physics-contacts spec 6.1): which messages a
// body listens to, and the Enter / Stay / Exit rules over the collider pairs that touch after
// each step. One ContactTracker per dimension per PhysicsSystem; the backends (Jolt, Box2D)
// tell it which pairs touch after a step, it answers with the events in dispatch order.

#include "Export.h"
#include "ecs/Entity.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace jasperengine {

    // The listener mask of a body: one bit per contact message a script of its GameObject (or
    // of a collider `receiver`) defines, in ScriptMessage order: bits 0-5 the 3D messages,
    // 6-11 the 2D ones. Pairs where no side has a bit are never recorded.
    namespace PhysicsCallbacks {
        // (ScriptMessage values 0-11; checked in ContactTracker.cpp)
        constexpr uint16_t COLLISION_3D = 0x007;   // CollisionEnter, Stay, Exit
        constexpr uint16_t TRIGGER_3D = 0x038;     // TriggerEnter, Stay, Exit
        constexpr uint16_t COLLISION_2D = 0x1C0;   // CollisionEnter2D, Stay2D, Exit2D
        constexpr uint16_t TRIGGER_2D = 0xE00;     // TriggerEnter2D, Stay2D, Exit2D
        constexpr uint16_t ALL_3D = COLLISION_3D | TRIGGER_3D;
        constexpr uint16_t ALL_2D = COLLISION_2D | TRIGGER_2D;

        inline uint16_t fromReceivers(uint32_t receivers) {
            return (uint16_t)(receivers & (ALL_3D | ALL_2D));
        }

        // Jolt body user data: the entity in the low 32 bits, the listener mask above, so
        // the job threads test the mask without touching the scene
        inline uint64_t packBodyUserData(Entity entity, uint16_t mask) {
            return (uint64_t)(uint32_t)entity | ((uint64_t)mask << 32);
        }
        inline Entity bodyEntity(uint64_t userData) {
            return (Entity)(uint32_t)(userData & 0xFFFFFFFFull);
        }
        inline uint16_t bodyMask(uint64_t userData) {
            return (uint16_t)(userData >> 32);
        }
    }

    // One collider of a body: the body's GameObject and the index in its colliders list (-1:
    // a collider removed while it touched)
    struct ColliderKey {
        Entity entity = NULL_ENTITY;
        int collider = -1;

        bool operator==(const ColliderKey& other) const { return entity == other.entity && collider == other.collider; }
        bool operator!=(const ColliderKey& other) const { return !(*this == other); }
        bool operator<(const ColliderKey& other) const {
            return entity != other.entity ? entity < other.entity : collider < other.collider;
        }
    };

    // A collider pair, a < b
    struct ColliderPairKey {
        ColliderKey a, b;

        static ColliderPairKey make(const ColliderKey& x, const ColliderKey& y) {
            return (y < x) ? ColliderPairKey{y, x} : ColliderPairKey{x, y};
        }
        bool operator==(const ColliderPairKey& other) const { return a == other.a && b == other.b; }
        bool operator<(const ColliderPairKey& other) const {
            return a != other.a ? a < other.a : b < other.b;
        }
    };

    enum class ContactPhase : uint8_t { Enter, Stay, Exit };

    class JASPERENGINE_API ContactTracker {
    public:
        // a pair touching after this step; `asleep`: kept although the backend did not report
        // it (its bodies sleep): no Stay. `data`: the backend's index of its contact data.
        struct Current {
            bool trigger = false;
            bool asleep = false;
            int data = -1;
        };
        using CurrentMap = std::map<ColliderPairKey, Current>;

        struct Pair {
            ColliderKey a, b;
            bool trigger = false;
            bool asleep = false;
            uint32_t sinceStep = 0;   // the step of its Enter
        };

        struct Event {
            ColliderKey a, b;
            bool trigger = false;
            ContactPhase phase = ContactPhase::Enter;
            int data = -1;            // Current::data of Enter and Stay, -1 for Exit
        };

        // The pairs touching after the step against the last ones: Enter (new), Stay (still
        // touching, unless asleep), Exit (gone). A pair whose trigger state changed exits and
        // enters again. Events in dispatch order (spec D10): triggers then collisions; Enter,
        // Stay, Exit; pairs sorted by key.
        std::vector<Event> update(const CurrentMap& now);

        // The pairs of a GameObject that goes (destroy, Rigidbody removed): removed now, for
        // the immediate Exit to the other sides (spec D6)
        std::vector<Pair> removeEntity(Entity entity);
        // A collider of the entity was removed: its pairs get index -1 (they end at the next
        // update), the later ones shift down, unless `renumber` is off (a generated 2D collider's
        // key is not a position in a list, so nothing follows it)
        void removeCollider(Entity entity, int index, bool renumber = true);

        const std::map<ColliderPairKey, Pair>& getPairs() const { return pairs; }
        std::vector<Pair> getPairsOf(Entity entity) const;
        bool hasPairsOf(Entity entity) const;

        // fixed steps updated so far (Pair::sinceStep)
        uint32_t getStep() const { return step; }

        // raw contact records the backend made in the last step for listened bodies or global
        // subscribers (none when nothing listens)
        size_t recordedCount() const { return recorded; }
        void setRecordedCount(size_t count) { recorded = count; }

        void clear();

    private:
        std::map<ColliderPairKey, Pair> pairs;
        uint32_t step = 0;
        size_t recorded = 0;
    };

}

#endif // CONTACT_TRACKER_H
