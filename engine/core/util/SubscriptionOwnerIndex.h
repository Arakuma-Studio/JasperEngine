// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Export.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace jasperengine {

    namespace detail {
        // The non-template side of a subscriber list (Signal, FunctionSubscribe): what a
        // Subscription and the owner index hold on to
        class JASPERENGINE_API SlotStateBase {
        public:
            virtual ~SlotStateBase();
            // Removes one subscriber (during an invoke: marks it, it runs no more). False if gone.
            virtual bool removeSlot(uint64_t id) = 0;
        };
    }

    // An owner key: the address of what the subscriptions and timers belong to (a script
    // instance, a Lua script's table)
    inline uint64_t ownerKeyOf(const void* owner) {
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(owner));
    }

    // owner key -> its subscriptions in every signal, so a script going away removes its
    // own subscriptions in O(its subscriptions), without scanning every event. Thread-safe.
    class JASPERENGINE_API SubscriptionOwnerIndex {
    public:
        static void add(uint64_t ownerKey, const std::shared_ptr<detail::SlotStateBase>& state, uint64_t id);
        // The subscription went away by other means
        static void forget(uint64_t ownerKey, const detail::SlotStateBase* state, uint64_t id);
        // Removes every subscription of the owner; returns how many were live
        static size_t removeOwner(uint64_t ownerKey);
        // removeOwner plus the owner's timers in every Scheduler: what a script's release
        // (and its hot reload) does
        static size_t releaseOwner(uint64_t ownerKey);
        // Subscriptions recorded for the owner (some may be gone already)
        static size_t entryCount(uint64_t ownerKey);

        // The owner a subscription made on this thread gets when its add() names none (the
        // Lua add(tag, function) path: RegisterEvent sets the calling script's table).
        // Returns the previous one, to restore.
        static uint64_t setRegistrationOwner(uint64_t ownerKey);
        static uint64_t registrationOwner();
    };

}
