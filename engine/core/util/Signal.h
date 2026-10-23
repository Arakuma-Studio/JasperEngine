// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "SubscriptionOwnerIndex.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine {

    // A subscription to a Signal (or FunctionSubscribe::subscribe): unsubscribes when
    // destroyed or reset. Move-only; inert when the signal went away first. Unsubscribing
    // does not wait for a callback running on another thread.
    class Subscription {
    public:
        Subscription() = default;
        Subscription(std::weak_ptr<detail::SlotStateBase> state, uint64_t id): state(std::move(state)), id(id) {}

        Subscription(Subscription&& other) noexcept: state(std::move(other.state)), id(other.id) {
            other.id = 0;
        }
        Subscription& operator=(Subscription&& other) noexcept {
            if (this != &other) {
                reset();
                state = std::move(other.state);
                id = other.id;
                other.id = 0;
            }
            return *this;
        }
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;

        ~Subscription() {
            reset();
        }

        // Unsubscribes now
        void reset() {
            if (id != 0) {
                if (std::shared_ptr<detail::SlotStateBase> live = state.lock()) live->removeSlot(id);
                state.reset();
                id = 0;
            }
        }

        // Keeps the subscription for the signal's lifetime (nothing unsubscribes it any more)
        void release() {
            state.reset();
            id = 0;
        }

        bool active() const {
            return id != 0 && !state.expired();
        }

    private:
        std::weak_ptr<detail::SlotStateBase> state;
        uint64_t id = 0;
    };

    namespace detail {

        // The subscriber list Signal and FunctionSubscribe share. Not thread-safe (the
        // owner serializes; hasSubscribers-style reads of the count are). Re-entrancy:
        // - an invoke goes over the subscribers there were when it started; one added
        //   meanwhile waits for the next invoke;
        // - removing (or clearing) during an invoke marks the subscriber: it is skipped if
        //   it did not run yet, and the one running stays alive until it returns;
        // - the list is compacted after the outermost invoke; nested invokes are fine;
        // - the owner (Signal) can go away during an invoke: the list lives until it ends.
        template<typename Signature>
        class SlotList;

        template<typename R, typename... Args>
        class SlotList<R(Args...)> final : public SlotStateBase, public std::enable_shared_from_this<SlotList<R(Args...)>> {
        public:
            using Function = std::function<R(Args...)>;

            struct Slot {
                uint64_t id;
                uint64_t ownerKey;
                std::string tag;
                Function function;
                bool removed;
            };

            ~SlotList() override {
                for (const Slot& slot : slots) {
                    if (!slot.removed && slot.ownerKey) SubscriptionOwnerIndex::forget(slot.ownerKey, this, slot.id);
                }
                for (const Slot& slot : pending) {
                    if (!slot.removed && slot.ownerKey) SubscriptionOwnerIndex::forget(slot.ownerKey, this, slot.id);
                }
            }

            uint64_t add(Function function, uint64_t ownerKey, std::string tag) {
                const uint64_t id = nextId++;
                Slot slot{id, ownerKey, std::move(tag), std::move(function), false};
                // a vector that grows would move the callback being invoked: wait outside
                if (depth > 0) {
                    pending.push_back(std::move(slot));
                } else {
                    slots.push_back(std::move(slot));
                }
                live.fetch_add(1, std::memory_order_relaxed);
                if (ownerKey) SubscriptionOwnerIndex::add(ownerKey, this->shared_from_this(), id);
                return id;
            }

            bool removeSlot(uint64_t id) override {
                return removeWhere([id](const Slot& slot) { return slot.id == id; }, true) > 0;
            }

            bool removeTag(const std::string& tag) {
                return removeWhere([&tag](const Slot& slot) { return slot.tag == tag; }, true) > 0;
            }

            size_t removeTagSubstring(const std::string& substring) {
                return removeWhere([&substring](const Slot& slot) { return slot.tag.find(substring) != std::string::npos; }, false);
            }

            size_t removeOwner(uint64_t ownerKey) {
                if (ownerKey == 0) return 0;
                return removeWhere([ownerKey](const Slot& slot) { return slot.ownerKey == ownerKey; }, false);
            }

            void clear() {
                removeWhere([](const Slot&) { return true; }, false);
            }

            size_t size() const {
                return live.load(std::memory_order_relaxed);
            }

            bool hasTag(const std::string& tag) const {
                for (const Slot& slot : slots) if (!slot.removed && slot.tag == tag) return true;
                for (const Slot& slot : pending) if (!slot.removed && slot.tag == tag) return true;
                return false;
            }

            // Calls visit(slot) for every subscriber at the start of the invoke that is still
            // subscribed; visit returns false to stop. Allocates nothing.
            template<typename Visit>
            void invoke(Visit&& visit) {
                Scope scope(*this);
                const size_t count = slots.size();
                for (size_t i = 0; i < count; i++) {
                    Slot& slot = slots[i];
                    if (slot.removed) continue;
                    if (!visit(slot)) break;
                }
            }

            // Copies the subscribers (a FunctionSubscribe copied with its component)
            std::shared_ptr<SlotList> clone() const {
                auto copy = std::make_shared<SlotList>();
                for (const Slot& slot : slots) {
                    if (!slot.removed) copy->add(slot.function, slot.ownerKey, slot.tag);
                }
                for (const Slot& slot : pending) {
                    if (!slot.removed) copy->add(slot.function, slot.ownerKey, slot.tag);
                }
                return copy;
            }

            // The owner goes away: during an invoke the list keeps itself alive until it ends
            void orphan() {
                if (depth > 0) keepAlive = this->shared_from_this();
            }

            bool invoking() const {
                return depth > 0;
            }

        private:
            std::vector<Slot> slots;
            std::vector<Slot> pending;      // added during an invoke
            uint32_t depth = 0;
            bool dirty = false;
            std::atomic<size_t> live{0};
            uint64_t nextId = 1;
            std::shared_ptr<SlotList> keepAlive;

            struct Scope {
                SlotList& list;
                explicit Scope(SlotList& list): list(list) {
                    list.depth++;
                }
                ~Scope() {
                    if (--list.depth == 0) {
                        // the last thing done with the list: it may delete itself here
                        std::shared_ptr<SlotList> self = std::move(list.keepAlive);
                        if (list.dirty || !list.pending.empty()) list.compact();
                    }
                }
            };

            template<typename Match>
            size_t removeWhere(Match&& match, bool firstOnly) {
                size_t removed = 0;
                auto visit = [&](std::vector<Slot>& from, bool inPlace) {
                    for (size_t i = 0; i < from.size(); i++) {
                        Slot& slot = from[i];
                        if (slot.removed || !match(slot)) continue;
                        slot.removed = true;
                        live.fetch_sub(1, std::memory_order_relaxed);
                        if (slot.ownerKey) SubscriptionOwnerIndex::forget(slot.ownerKey, this, slot.id);
                        removed++;
                        if (inPlace) dirty = true;
                        if (firstOnly) return true;
                    }
                    return false;
                };
                if (!visit(slots, true) && !(firstOnly && removed)) visit(pending, true);
                if (depth == 0 && dirty) compact();
                return removed;
            }

            void compact() {
                size_t write = 0;
                for (size_t read = 0; read < slots.size(); read++) {
                    if (slots[read].removed) continue;
                    if (write != read) slots[write] = std::move(slots[read]);
                    write++;
                }
                slots.resize(write);
                for (Slot& slot : pending) {
                    if (!slot.removed) slots.push_back(std::move(slot));
                }
                pending.clear();
                dirty = false;
            }
        };

    }

    // An event for new C++ code: subscribers hold Subscriptions (RAII). Externally
    // synchronized: one thread at a time subscribes/invokes (any thread may invoke when the
    // owner serializes it). See detail::SlotList for re-entrancy.
    template<typename... Args>
    class Signal {
    public:
        using Function = std::function<void(Args...)>;

        Signal() = default;
        Signal(const Signal&) = delete;
        Signal& operator=(const Signal&) = delete;
        Signal(Signal&& other) noexcept: state(std::move(other.state)) {}
        Signal& operator=(Signal&& other) noexcept {
            if (this != &other) {
                if (state) state->orphan();
                state = std::move(other.state);
            }
            return *this;
        }
        ~Signal() {
            if (state) state->orphan();
        }

        // ownerKey (ownerKeyOf(this)) lets SubscriptionOwnerIndex::removeOwner find it
        [[nodiscard]] Subscription subscribe(Function function, uint64_t ownerKey = 0) {
            const uint64_t id = list().add(std::move(function), ownerKey, std::string());
            return Subscription(std::weak_ptr<detail::SlotStateBase>(state), id);
        }

        void invoke(Args... args) {
            if (!state) return;
            // the list, not the member: the signal may be destroyed by a subscriber
            detail::SlotList<void(Args...)>* slots = state.get();
            slots->invoke([&](auto& slot) {
                slot.function(args...);
                return true;
            });
        }

        void operator()(Args... args) {
            invoke(args...);
        }

        size_t size() const {
            return state ? state->size() : 0;
        }

        bool empty() const {
            return size() == 0;
        }

        void clear() {
            if (state) state->clear();
        }

        size_t removeOwner(uint64_t ownerKey) {
            return state ? state->removeOwner(ownerKey) : 0;
        }

    private:
        std::shared_ptr<detail::SlotList<void(Args...)>> state;

        detail::SlotList<void(Args...)>& list() {
            if (!state) state = std::make_shared<detail::SlotList<void(Args...)>>();
            return *state;
        }
    };

    // A Signal any thread may subscribe to, unsubscribe from and invoke at the same time
    // (editor services published from background threads). Invoking takes a snapshot of
    // the subscribers (copy-on-write: subscribing and unsubscribing allocate, invoking
    // does not); one unsubscribed before its turn is skipped.
    template<typename... Args>
    class ThreadSafeSignal {
    public:
        using Function = std::function<void(Args...)>;

        ThreadSafeSignal(): state(std::make_shared<State>()) {}
        ThreadSafeSignal(const ThreadSafeSignal&) = delete;
        ThreadSafeSignal& operator=(const ThreadSafeSignal&) = delete;

        [[nodiscard]] Subscription subscribe(Function function, uint64_t ownerKey = 0) {
            const uint64_t id = state->add(std::move(function), ownerKey);
            return Subscription(std::weak_ptr<detail::SlotStateBase>(state), id);
        }

        void invoke(Args... args) {
            std::shared_ptr<const Entries> snapshot = state->snapshot();
            for (const std::shared_ptr<Entry>& entry : *snapshot) {
                if (entry->removed.load(std::memory_order_acquire)) continue;
                entry->function(args...);
            }
        }

        void operator()(Args... args) {
            invoke(args...);
        }

        size_t size() const {
            return state->snapshot()->size();
        }

        bool empty() const {
            return size() == 0;
        }

        void clear() {
            state->clear();
        }

        size_t removeOwner(uint64_t ownerKey) {
            return state->removeOwner(ownerKey);
        }

    private:
        struct Entry {
            uint64_t id;
            uint64_t ownerKey;
            Function function;
            std::atomic<bool> removed{false};
        };
        using Entries = std::vector<std::shared_ptr<Entry>>;

        class State final : public detail::SlotStateBase, public std::enable_shared_from_this<State> {
        public:
            uint64_t add(Function function, uint64_t ownerKey) {
                auto entry = std::make_shared<Entry>();
                entry->ownerKey = ownerKey;
                entry->function = std::move(function);
                uint64_t id;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    id = entry->id = nextId++;
                    auto next = std::make_shared<Entries>(*entries);
                    next->push_back(std::move(entry));
                    entries = std::move(next);
                }
                if (ownerKey) SubscriptionOwnerIndex::add(ownerKey, this->shared_from_this(), id);
                return id;
            }

            bool removeSlot(uint64_t id) override {
                return removeWhere([id](const Entry& entry) { return entry.id == id; }) > 0;
            }

            size_t removeOwner(uint64_t ownerKey) {
                return removeWhere([ownerKey](const Entry& entry) { return entry.ownerKey == ownerKey; });
            }

            void clear() {
                removeWhere([](const Entry&) { return true; });
            }

            std::shared_ptr<const Entries> snapshot() const {
                std::lock_guard<std::mutex> lock(mutex);
                return entries;
            }

        private:
            mutable std::mutex mutex;
            std::shared_ptr<const Entries> entries = std::make_shared<Entries>();
            uint64_t nextId = 1;

            template<typename Match>
            size_t removeWhere(Match&& match) {
                std::vector<std::pair<uint64_t, uint64_t>> forgotten;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    auto next = std::make_shared<Entries>();
                    for (const std::shared_ptr<Entry>& entry : *entries) {
                        if (match(*entry)) {
                            entry->removed.store(true, std::memory_order_release);
                            forgotten.emplace_back(entry->ownerKey, entry->id);
                        } else {
                            next->push_back(entry);
                        }
                    }
                    if (forgotten.empty()) return 0;
                    entries = std::move(next);
                }
                for (const auto& [ownerKey, id] : forgotten) {
                    if (ownerKey) SubscriptionOwnerIndex::forget(ownerKey, this, id);
                }
                return forgotten.size();
            }
        };

        std::shared_ptr<State> state;
    };

}
