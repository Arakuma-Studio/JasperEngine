// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ContactTracker.h"

#include "script/ScriptMessages.h"

#include <algorithm>

using namespace jasperengine;

static_assert(PhysicsCallbacks::COLLISION_3D == (scriptMessageBit(ScriptMessage::CollisionEnter) | scriptMessageBit(ScriptMessage::CollisionStay) | scriptMessageBit(ScriptMessage::CollisionExit)), "3D collision bits");
static_assert(PhysicsCallbacks::TRIGGER_3D == (scriptMessageBit(ScriptMessage::TriggerEnter) | scriptMessageBit(ScriptMessage::TriggerStay) | scriptMessageBit(ScriptMessage::TriggerExit)), "3D trigger bits");
static_assert(PhysicsCallbacks::COLLISION_2D == (scriptMessageBit(ScriptMessage::CollisionEnter2D) | scriptMessageBit(ScriptMessage::CollisionStay2D) | scriptMessageBit(ScriptMessage::CollisionExit2D)), "2D collision bits");
static_assert(PhysicsCallbacks::TRIGGER_2D == (scriptMessageBit(ScriptMessage::TriggerEnter2D) | scriptMessageBit(ScriptMessage::TriggerStay2D) | scriptMessageBit(ScriptMessage::TriggerExit2D)), "2D trigger bits");

namespace {

    int phaseOrder(const ContactTracker::Event& event) {
        return (event.trigger ? 0 : 3) + (int)event.phase;
    }

}

std::vector<ContactTracker::Event> ContactTracker::update(const CurrentMap& now) {
    step++;
    std::vector<Event> events;

    // pairs are both sorted: one merge walk
    auto previous = pairs.begin();
    auto current = now.begin();
    std::map<ColliderPairKey, Pair> next;
    while (previous != pairs.end() || current != now.end()) {
        const bool takePrevious = current == now.end() || (previous != pairs.end() && previous->first < current->first);
        const bool takeCurrent = previous == pairs.end() || (current != now.end() && current->first < previous->first);
        if (takePrevious) {
            const Pair& pair = previous->second;
            events.push_back({pair.a, pair.b, pair.trigger, ContactPhase::Exit, -1});
            ++previous;
            continue;
        }
        if (takeCurrent) {
            const ColliderPairKey& key = current->first;
            const Current& state = current->second;
            events.push_back({key.a, key.b, state.trigger, ContactPhase::Enter, state.data});
            next[key] = Pair{key.a, key.b, state.trigger, state.asleep, step};
            ++current;
            continue;
        }
        // in both
        const ColliderPairKey& key = current->first;
        const Current& state = current->second;
        Pair pair = previous->second;
        if (pair.trigger != state.trigger) {
            // a collider became a trigger (or stopped being one): a new pair
            events.push_back({pair.a, pair.b, pair.trigger, ContactPhase::Exit, -1});
            events.push_back({key.a, key.b, state.trigger, ContactPhase::Enter, state.data});
            pair.trigger = state.trigger;
            pair.sinceStep = step;
        } else if (!state.asleep) {
            events.push_back({key.a, key.b, state.trigger, ContactPhase::Stay, state.data});
        }
        pair.asleep = state.asleep;
        next[key] = pair;
        ++previous;
        ++current;
    }
    pairs.swap(next);

    // pairs were visited in key order: a stable sort keeps it inside each group
    std::stable_sort(events.begin(), events.end(), [](const Event& x, const Event& y) {
        return phaseOrder(x) < phaseOrder(y);
    });
    return events;
}

std::vector<ContactTracker::Pair> ContactTracker::removeEntity(Entity entity) {
    std::vector<Pair> removed;
    for (auto it = pairs.begin(); it != pairs.end();) {
        if (it->first.a.entity == entity || it->first.b.entity == entity) {
            removed.push_back(it->second);
            it = pairs.erase(it);
        } else {
            ++it;
        }
    }
    return removed;
}

void ContactTracker::removeCollider(Entity entity, int index, bool renumber) {
    std::map<ColliderPairKey, Pair> next;
    auto renumbered = [&](ColliderKey key) {
        if (key.entity == entity && key.collider >= 0) {
            if (key.collider == index) {
                key.collider = -1;
            } else if (renumber && key.collider > index) {
                key.collider--;
            }
        }
        return key;
    };
    for (auto& [key, pair] : pairs) {
        Pair moved = pair;
        moved.a = renumbered(pair.a);
        moved.b = renumbered(pair.b);
        const ColliderPairKey movedKey = ColliderPairKey::make(moved.a, moved.b);
        moved.a = movedKey.a;
        moved.b = movedKey.b;
        next[movedKey] = moved;
    }
    pairs.swap(next);
}

std::vector<ContactTracker::Pair> ContactTracker::getPairsOf(Entity entity) const {
    std::vector<Pair> result;
    for (const auto& [key, pair] : pairs) {
        if (key.a.entity == entity || key.b.entity == entity) {
            result.push_back(pair);
        }
    }
    return result;
}

bool ContactTracker::hasPairsOf(Entity entity) const {
    for (const auto& [key, pair] : pairs) {
        if (key.a.entity == entity || key.b.entity == entity) {
            return true;
        }
    }
    return false;
}

void ContactTracker::clear() {
    pairs.clear();
    recorded = 0;
}
