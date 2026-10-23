// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SubscriptionOwnerIndex.h"
#include "Scheduler.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>

using namespace jasperengine;

namespace {

    struct Entry {
        std::weak_ptr<detail::SlotStateBase> state;
        const detail::SlotStateBase* raw;
        uint64_t id;
    };

    struct IndexState {
        std::mutex mutex;
        std::unordered_map<uint64_t, std::vector<Entry>> owners;
    };

    IndexState& indexState() {
        static IndexState* state = new IndexState();
        return *state;
    }

}

detail::SlotStateBase::~SlotStateBase() = default;

void SubscriptionOwnerIndex::add(uint64_t ownerKey, const std::shared_ptr<detail::SlotStateBase>& state, uint64_t id) {
    if (ownerKey == 0 || !state) return;
    IndexState& index = indexState();
    std::lock_guard<std::mutex> lock(index.mutex);
    std::vector<Entry>& entries = index.owners[ownerKey];
    // signals that went away leave expired entries: dropped as the list grows
    if (entries.size() >= 16 && (entries.size() & (entries.size() - 1)) == 0) {
        entries.erase(std::remove_if(entries.begin(), entries.end(), [](const Entry& e) { return e.state.expired(); }),
                      entries.end());
    }
    entries.push_back(Entry{state, state.get(), id});
}

void SubscriptionOwnerIndex::forget(uint64_t ownerKey, const detail::SlotStateBase* state, uint64_t id) {
    if (ownerKey == 0) return;
    IndexState& index = indexState();
    std::lock_guard<std::mutex> lock(index.mutex);
    auto it = index.owners.find(ownerKey);
    if (it == index.owners.end()) return;
    std::vector<Entry>& entries = it->second;
    for (size_t i = 0; i < entries.size(); i++) {
        if (entries[i].raw == state && entries[i].id == id) {
            entries[i] = std::move(entries.back());
            entries.pop_back();
            break;
        }
    }
    if (entries.empty()) index.owners.erase(it);
}

size_t SubscriptionOwnerIndex::removeOwner(uint64_t ownerKey) {
    if (ownerKey == 0) return 0;
    std::vector<Entry> entries;
    {
        IndexState& index = indexState();
        std::lock_guard<std::mutex> lock(index.mutex);
        auto it = index.owners.find(ownerKey);
        if (it == index.owners.end()) return 0;
        entries = std::move(it->second);
        index.owners.erase(it);
    }
    // outside the lock: removing a slot calls forget()
    size_t removed = 0;
    for (const Entry& entry : entries) {
        if (std::shared_ptr<detail::SlotStateBase> state = entry.state.lock()) {
            if (state->removeSlot(entry.id)) removed++;
        }
    }
    return removed;
}

size_t SubscriptionOwnerIndex::releaseOwner(uint64_t ownerKey) {
    const size_t removed = removeOwner(ownerKey);
    Scheduler::cancelOwnerEverywhere(ownerKey);
    return removed;
}

size_t SubscriptionOwnerIndex::entryCount(uint64_t ownerKey) {
    IndexState& index = indexState();
    std::lock_guard<std::mutex> lock(index.mutex);
    auto it = index.owners.find(ownerKey);
    return it == index.owners.end() ? 0 : it->second.size();
}

namespace {
    uint64_t& threadRegistrationOwner() {
        thread_local uint64_t owner = 0;
        return owner;
    }
}

uint64_t SubscriptionOwnerIndex::setRegistrationOwner(uint64_t ownerKey) {
    uint64_t& owner = threadRegistrationOwner();
    const uint64_t previous = owner;
    owner = ownerKey;
    return previous;
}

uint64_t SubscriptionOwnerIndex::registrationOwner() {
    return threadRegistrationOwner();
}
