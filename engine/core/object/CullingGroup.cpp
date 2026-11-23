// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CullingGroup.h"

#include "Log.h"
#include "Scene.h"
#include "component/Transform.h"
#include "subsystem/CullingSystem.h"
#include "util/SubscriptionOwnerIndex.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace {

    constexpr uint32_t STATE_MASK = 0xFFu;
    constexpr uint32_t VISIBLE_BIT = 0x80u;
    constexpr uint32_t BAND_MASK = 0x0Fu;
    constexpr uint32_t BAND_UNKNOWN = 0x0Fu;

    CullingSystem::GroupSlot* groupOf(const CullingGroup& handle, const char* function) {
        Scene* scene = handle.getScene();
        CullingSystem* system = scene ? scene->findSystem<CullingSystem>().get() : nullptr;
        CullingSystem::GroupSlot* group = system ? system->findGroup(handle.getSlot(), handle.getGeneration()) : nullptr;
        if (!group && function) {
            // once per call site that keeps using a disposed group would flood: once per process
            static bool warned = false;
            if (!warned) {
                warned = true;
                Log::warn("CullingGroup.%s: the group was disposed (or never made); nothing is done", function);
            }
        }
        return group;
    }

    bool validSphere(const BoundingSphere& sphere) {
        return std::isfinite(sphere.position.x) && std::isfinite(sphere.position.y) && std::isfinite(sphere.position.z) &&
               std::isfinite(sphere.radius) && sphere.radius >= 0.0f;
    }

    void dropFollow(CullingSystem::GroupSlot& group, uint32_t index) {
        const uint32_t slot = group.meta[index] >> 8;
        if (slot == 0) return;
        const size_t f = slot - 1;
        group.meta[index] &= STATE_MASK;
        if (f + 1 != group.follows.size()) {
            group.follows[f] = group.follows.back();
            const uint32_t moved = group.follows[f].index;
            group.meta[moved] = (group.meta[moved] & STATE_MASK) | (static_cast<uint32_t>(f + 1) << 8);
        }
        group.follows.pop_back();
    }

    void resize(CullingSystem::GroupSlot& group, uint32_t count) {
        // the follows of the spheres that go
        for (size_t f = 0; f < group.follows.size();) {
            if (group.follows[f].index >= count) {
                dropFollow(group, group.follows[f].index);
            } else {
                f++;
            }
        }
        group.x.resize(count, 0.0f);
        group.y.resize(count, 0.0f);
        group.z.resize(count, 0.0f);
        group.r.resize(count, 0.0f);
        group.meta.resize(count, BAND_UNKNOWN);
    }

    bool checkIndex(const CullingSystem::GroupSlot& group, uint32_t index, const char* function) {
        if (index < group.x.size()) return true;
        Log::error("CullingGroup.%s: sphere %u is past the group's %zu spheres", function, index, group.x.size());
        return false;
    }

    template<typename Match>
    uint32_t query(const CullingSystem::GroupSlot* group, std::span<uint32_t> out, uint32_t first, Match match) {
        if (!group) return 0;
        uint32_t written = 0, skipped = 0;
        for (uint32_t i = 0; i < group->meta.size() && written < out.size(); i++) {
            if (!match(group->meta[i])) continue;
            if (skipped < first) {
                skipped++;
                continue;
            }
            out[written++] = i;
        }
        return written;
    }

}

CullingGroup::CullingGroup(Scene* scene) {
    *this = create(scene);
}

CullingGroup::CullingGroup(Scene* scene, uint32_t slot, uint32_t generation)
    : scene(scene), sceneLifetime(scene ? scene->getLifetime() : std::weak_ptr<char>()), slot(slot), generation(generation) {}

CullingGroup CullingGroup::create(Scene* scene) {
    if (!scene) return CullingGroup();
    CullingGroup group = scene->getSystem<CullingSystem>()->createGroup();
    // made inside a Lua script's callback: it goes when that script instance is released
    if (group.isValid()) {
        scene->getSystem<CullingSystem>()->setGroupOwner(group.slot, group.generation, SubscriptionOwnerIndex::scriptOwner());
    }
    return group;
}

void CullingGroup::dispose() {
    if (!groupOf(*this, "dispose")) return;
    scene->getSystem<CullingSystem>()->disposeGroup(slot, generation);
}

bool CullingGroup::isValid() const {
    return groupOf(*this, nullptr) != nullptr;
}

void CullingGroup::setEnabled(bool enabled) {
    if (CullingSystem::GroupSlot* group = groupOf(*this, "enabled")) group->enabled = enabled;
}

bool CullingGroup::isEnabled() const {
    const CullingSystem::GroupSlot* group = groupOf(*this, nullptr);
    return group && group->enabled;
}

void CullingGroup::setTargetCamera(Entity camera) {
    if (CullingSystem::GroupSlot* group = groupOf(*this, "targetCamera")) group->targetCamera = camera;
}

Entity CullingGroup::getTargetCamera() const {
    const CullingSystem::GroupSlot* group = groupOf(*this, nullptr);
    return group ? group->targetCamera : NULL_ENTITY;
}

void CullingGroup::setBoundingSpheres(std::span<const BoundingSphere> spheres) {
    CullingSystem::GroupSlot* group = groupOf(*this, "setBoundingSpheres");
    if (!group) return;
    if (spheres.size() > MAX_SPHERES) {
        Log::error("CullingGroup.setBoundingSpheres: %zu spheres; a group has at most %u", spheres.size(), MAX_SPHERES);
        return;
    }
    for (const BoundingSphere& sphere : spheres) {
        if (!validSphere(sphere)) {
            Log::error("CullingGroup.setBoundingSpheres: a sphere's position must be finite and its radius finite and >= 0");
            return;
        }
    }
    resize(*group, static_cast<uint32_t>(spheres.size()));
    // copied: the spheres stop following
    while (!group->follows.empty()) dropFollow(*group, group->follows.back().index);
    for (size_t i = 0; i < spheres.size(); i++) {
        group->x[i] = spheres[i].position.x;
        group->y[i] = spheres[i].position.y;
        group->z[i] = spheres[i].position.z;
        group->r[i] = spheres[i].radius;
    }
}

void CullingGroup::setBoundingSphereCount(uint32_t count) {
    CullingSystem::GroupSlot* group = groupOf(*this, "setBoundingSphereCount");
    if (!group) return;
    if (count > MAX_SPHERES) {
        Log::error("CullingGroup.setBoundingSphereCount: %u spheres; a group has at most %u", count, MAX_SPHERES);
        return;
    }
    resize(*group, count);
}

uint32_t CullingGroup::getBoundingSphereCount() const {
    const CullingSystem::GroupSlot* group = groupOf(*this, nullptr);
    return group ? static_cast<uint32_t>(group->x.size()) : 0;
}

void CullingGroup::setSphere(uint32_t index, const BoundingSphere& sphere) {
    CullingSystem::GroupSlot* group = groupOf(*this, "setSphere");
    if (!group || !checkIndex(*group, index, "setSphere")) return;
    if (!validSphere(sphere)) {
        Log::error("CullingGroup.setSphere: the position must be finite and the radius finite and >= 0");
        return;
    }
    dropFollow(*group, index);
    group->x[index] = sphere.position.x;
    group->y[index] = sphere.position.y;
    group->z[index] = sphere.position.z;
    group->r[index] = sphere.radius;
}

BoundingSphere CullingGroup::getSphere(uint32_t index) const {
    const CullingSystem::GroupSlot* group = groupOf(*this, nullptr);
    if (!group || index >= group->x.size()) return BoundingSphere();
    return BoundingSphere(Vector3(group->x[index], group->y[index], group->z[index]), group->r[index]);
}

void CullingGroup::setSphereTarget(uint32_t index, Entity entity, float radius, const Vector3& offset) {
    CullingSystem::GroupSlot* group = groupOf(*this, "setSphereTarget");
    if (!group || !checkIndex(*group, index, "setSphereTarget")) return;
    if (!std::isfinite(radius) || radius < 0.0f || !std::isfinite(offset.x) || !std::isfinite(offset.y) || !std::isfinite(offset.z)) {
        Log::error("CullingGroup.setSphereTarget: the radius must be finite and >= 0 and the offset finite");
        return;
    }
    if (!scene->isEntityCreated(entity)) {
        Log::error("CullingGroup.setSphereTarget: GameObject %u does not exist", entity);
        return;
    }
    group->r[index] = radius;
    const uint32_t slot = group->meta[index] >> 8;
    if (slot) {
        group->follows[slot - 1].entity = entity;
        group->follows[slot - 1].offset = offset;
    } else {
        // doubling on every standard library (MSVC's grows by half, libstdc++'s doubles), so the memory
        // getMemoryBytes reports by capacity (R24) is the same on every platform
        if (group->follows.size() == group->follows.capacity())
            group->follows.reserve(std::max<size_t>(1, group->follows.capacity() * 2));
        group->follows.push_back({index, entity, offset});
        group->meta[index] = (group->meta[index] & STATE_MASK) | (static_cast<uint32_t>(group->follows.size()) << 8);
    }
    // where it is now, until the next evaluation follows it
    if (const Transform* transform = scene->findComponent<Transform>(entity)) {
        group->x[index] = transform->worldPosition.x + offset.x;
        group->y[index] = transform->worldPosition.y + offset.y;
        group->z[index] = transform->worldPosition.z + offset.z;
    }
}

void CullingGroup::eraseSwapBack(uint32_t index) {
    CullingSystem::GroupSlot* group = groupOf(*this, "eraseSwapBack");
    if (!group || !checkIndex(*group, index, "eraseSwapBack")) return;
    dropFollow(*group, index);
    const uint32_t last = static_cast<uint32_t>(group->x.size()) - 1;
    if (index != last) {
        group->x[index] = group->x[last];
        group->y[index] = group->y[last];
        group->z[index] = group->z[last];
        group->r[index] = group->r[last];
        group->meta[index] = group->meta[last];
        const uint32_t slot = group->meta[index] >> 8;
        if (slot) group->follows[slot - 1].index = index;
    }
    group->x.pop_back();
    group->y.pop_back();
    group->z.pop_back();
    group->r.pop_back();
    group->meta.pop_back();
}

void CullingGroup::setBoundingDistances(std::span<const float> distances) {
    CullingSystem::GroupSlot* group = groupOf(*this, "setBoundingDistances");
    if (!group) return;
    if (distances.size() > MAX_DISTANCES) {
        Log::error("CullingGroup.setBoundingDistances: %zu distances; at most %zu", distances.size(), MAX_DISTANCES);
        return;
    }
    for (size_t i = 0; i < distances.size(); i++) {
        if (!std::isfinite(distances[i]) || distances[i] <= 0.0f || (i > 0 && distances[i] <= distances[i - 1])) {
            Log::error("CullingGroup.setBoundingDistances: distances must be finite, positive and ascending");
            return;
        }
    }
    group->distanceCount = static_cast<uint8_t>(distances.size());
    for (size_t i = 0; i < distances.size(); i++) group->distances[i] = distances[i];
}

void CullingGroup::setDistanceReferencePoint(const Vector3& point) {
    CullingSystem::GroupSlot* group = groupOf(*this, "setDistanceReferencePoint");
    if (!group) return;
    group->hasReferencePoint = true;
    group->referencePoint = point;
    group->referenceEntity = NULL_ENTITY;
}

void CullingGroup::setDistanceReferencePoint(Entity entity) {
    CullingSystem::GroupSlot* group = groupOf(*this, "setDistanceReferencePoint");
    if (!group) return;
    group->hasReferencePoint = false;
    group->referenceEntity = entity;
}

void CullingGroup::setBandHysteresis(float metres) {
    CullingSystem::GroupSlot* group = groupOf(*this, "bandHysteresis");
    if (!group) return;
    group->hysteresis = std::isfinite(metres) ? std::max(0.0f, metres) : 0.0f;
}

float CullingGroup::getBandHysteresis() const {
    const CullingSystem::GroupSlot* group = groupOf(*this, nullptr);
    return group ? group->hysteresis : 0.0f;
}

bool CullingGroup::isVisible(uint32_t index) const {
    const CullingSystem::GroupSlot* group = groupOf(*this, nullptr);
    return group && index < group->meta.size() && (group->meta[index] & VISIBLE_BIT) != 0;
}

int CullingGroup::getDistance(uint32_t index) const {
    const CullingSystem::GroupSlot* group = groupOf(*this, nullptr);
    if (!group || index >= group->meta.size()) return -1;
    const uint32_t band = group->meta[index] & BAND_MASK;
    return band == BAND_UNKNOWN ? -1 : static_cast<int>(band);
}

uint32_t CullingGroup::queryIndices(bool visible, std::span<uint32_t> out, uint32_t first) const {
    return query(groupOf(*this, nullptr), out, first, [visible](uint32_t meta) {
        return ((meta & VISIBLE_BIT) != 0) == visible;
    });
}

uint32_t CullingGroup::queryIndices(int band, std::span<uint32_t> out, uint32_t first) const {
    return query(groupOf(*this, nullptr), out, first, [band](uint32_t meta) {
        const uint32_t b = meta & BAND_MASK;
        return b != BAND_UNKNOWN && static_cast<int>(b) == band;
    });
}

uint32_t CullingGroup::queryIndices(bool visible, int band, std::span<uint32_t> out, uint32_t first) const {
    return query(groupOf(*this, nullptr), out, first, [visible, band](uint32_t meta) {
        const uint32_t b = meta & BAND_MASK;
        return ((meta & VISIBLE_BIT) != 0) == visible && b != BAND_UNKNOWN && static_cast<int>(b) == band;
    });
}

FunctionSubscribe<void(CullingGroupEvent)>& CullingGroup::onStateChanged() {
    CullingSystem::GroupSlot* group = groupOf(*this, "onStateChanged");
    if (!group) {
        // a disposed group's: subscriptions to it are never called
        static FunctionSubscribe<void(CullingGroupEvent)> none;
        none.clear();
        return none;
    }
    if (!group->onStateChanged) group->onStateChanged = std::make_unique<FunctionSubscribe<void(CullingGroupEvent)>>();
    return *group->onStateChanged;
}
