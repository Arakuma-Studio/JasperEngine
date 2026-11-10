// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VoxelChangeSet.h"

#include <algorithm>

using namespace jasperengine::voxel;

ChangeSetScheduler::ChangeSetScheduler(const ChangeSetOptions& opts): options(opts) {}

uint64_t ChangeSetScheduler::windowVersionOf(const BlockKey& key) const {
    const auto found = windowVersions.find(key);
    return found == windowVersions.end() ? 0 : found->second;
}

MeshId ChangeSetScheduler::idFor(const BlockKey& key, uint8_t sideMask) const {
    return MeshId{key, sideMask, windowVersionOf(key)};
}

const std::map<BlockKey, MeshId>& ChangeSetScheduler::latestTarget() const {
    return sets.empty() ? current : sets.back().target;
}

void ChangeSetScheduler::request(const MeshId& id, bool editPriority) {
    MeshEntry& entry = meshes[id];
    entry.lastUse = ++clock;
    if (entry.built || entry.requested) return;
    entry.requested = true;
    pendingRequests.push_back(MeshBuildRequest{id, editPriority});
}

void ChangeSetScheduler::startSet(bool isEdit, std::map<BlockKey, MeshId> target, const IntBox& changed) {
    ChangeSet set;
    set.id = nextSetId++;
    set.isEdit = isEdit;
    set.target = std::move(target);
    set.changed = changed;
    for (const auto& item : set.target) {
        // What is on screen under the same identity needs nothing: it is its own mesh
        const auto shown = current.find(item.first);
        if (shown != current.end() && shown->second == item.second) continue;
        request(item.second, isEdit);
    }
    sets.push_back(std::move(set));
}

void ChangeSetScheduler::setDesiredLod(const std::vector<VisibleBlock>& desired) {
    std::map<BlockKey, MeshId> target;
    for (const VisibleBlock& block : desired) target.emplace(block.key, idFor(block.key, block.sideMask));
    // Sets activate in order, so the newest target is what this one will replace
    if (!sets.empty()) {
        if (sets.back().target == target) {
            waitingLod = false;
            return;
        }
        waitingDesired = desired;
        waitingLod = true;
        return;
    }
    if (target == current && anyActivated()) {
        waitingLod = false;
        return;
    }
    waitingLod = false;
    startSet(false, std::move(target), IntBox{});
}

void ChangeSetScheduler::startWaitingLod() {
    if (!waitingLod || !sets.empty()) return;
    const std::vector<VisibleBlock> desired = std::move(waitingDesired);
    waitingDesired.clear();
    waitingLod = false;
    setDesiredLod(desired);
}

void ChangeSetScheduler::markDirty(const IntBox& changed, int levels) {
    editVersion++;
    for (int level = 0; level < levels; level++) {
        const int spacing = 1 << level;
        const int span = BLOCK_SIZE << level;
        const int pad = options.dirtyPadding * spacing;
        // A block's window holds samples [origin - pad, origin + span + pad]; at padding 0 it is only
        // its own 16 samples, which is the red control's missing padding
        const int reach = options.dirtyPadding > 0 ? span + pad : span - spacing;
        const auto firstBlock = [&](int low) {
            int block = (low - reach) / span;
            while ((block + 1) * span + 0 <= low - reach) block++;
            return std::max(block - 1, 0);
        };
        for (int z = firstBlock(changed.min.z); z * span - pad <= changed.max.z; z++) {
            if (z * span + reach < changed.min.z) continue;
            for (int y = firstBlock(changed.min.y); y * span - pad <= changed.max.y; y++) {
                if (y * span + reach < changed.min.y) continue;
                for (int x = firstBlock(changed.min.x); x * span - pad <= changed.max.x; x++) {
                    if (x * span + reach < changed.min.x) continue;
                    windowVersions[BlockKey{level, x, y, z}] = editVersion;
                }
            }
        }
    }
}

uint64_t ChangeSetScheduler::onEdit(const IntBox& changed, int levels) {
    if (changed.empty()) return lastActivated;
    // An edit pre-empts: an LOD set that has not activated goes, its finished meshes stay in the cache
    waitingLod = false;
    for (size_t i = sets.size(); i > 0; i--) {
        if (sets[i - 1].isEdit) continue;
        for (const auto& item : sets[i - 1].target) {
            const auto entry = meshes.find(item.second);
            if (entry != meshes.end() && entry->second.requested && !entry->second.built) {
                entry->second.requested = false;
                cancelled.push_back(item.second);
            }
        }
        sets.erase(sets.begin() + static_cast<std::ptrdiff_t>(i - 1));
    }
    markDirty(changed, levels);
    lodRefresh = true;
    if (sets.size() >= 2) {
        // A third set merges into the second
        ChangeSet& second = sets.back();
        second.changed.include(changed);
        for (auto& item : second.target) {
            const MeshId updated = idFor(item.first, item.second.sideMask);
            if (updated == item.second) continue;
            item.second = updated;
            request(updated, true);
        }
        return second.id;
    }
    std::map<BlockKey, MeshId> target;
    for (const auto& item : latestTarget()) target.emplace(item.first, idFor(item.first, item.second.sideMask));
    // A first set not yet visible at all still has to build at the new version
    startSet(true, std::move(target), changed);
    return sets.back().id;
}

void ChangeSetScheduler::takeRequests(std::vector<MeshBuildRequest>& out) {
    out.swap(pendingRequests);
    pendingRequests.clear();
}

void ChangeSetScheduler::takeCancelled(std::vector<MeshId>& out) {
    out.swap(cancelled);
    cancelled.clear();
}

void ChangeSetScheduler::meshBuilt(const MeshId& id, std::shared_ptr<const MeshOut> mesh) {
    MeshEntry& entry = meshes[id];
    entry.mesh = std::move(mesh);
    entry.built = true;
    entry.requested = false;
    entry.lastUse = ++clock;
}

void ChangeSetScheduler::meshFailed(const MeshId& id) {
    const auto found = meshes.find(id);
    if (found == meshes.end()) return;
    found->second.requested = false;
    found->second.built = false;
    for (const ChangeSet& set : sets) {
        for (const auto& item : set.target) {
            if (item.second == id) {
                request(id, set.isEdit);
                return;
            }
        }
    }
}

void ChangeSetScheduler::takeUploads(std::vector<std::pair<MeshId, std::shared_ptr<const MeshOut>>>& out) {
    out.clear();
    if (sets.empty()) return;
    // The front set first: it is what the screen waits for
    for (const ChangeSet& set : sets) {
        for (const auto& item : set.target) {
            const auto found = meshes.find(item.second);
            if (found == meshes.end()) continue;
            MeshEntry& entry = found->second;
            if (!entry.built || entry.uploaded || entry.uploadHandedOut) continue;
            if (!entry.mesh || entry.mesh->empty()) {
                // A block with no triangles has no chunk (R19): nothing to upload
                entry.uploaded = true;
                continue;
            }
            entry.uploadHandedOut = true;
            out.emplace_back(item.second, entry.mesh);
        }
    }
}

void ChangeSetScheduler::meshUploaded(const MeshId& id) {
    const auto found = meshes.find(id);
    if (found == meshes.end()) return;
    found->second.uploaded = true;
    found->second.uploadHandedOut = false;
}

void ChangeSetScheduler::meshReleased(const MeshId& id) {
    const auto found = meshes.find(id);
    if (found == meshes.end()) return;
    found->second.uploaded = false;
    found->second.uploadHandedOut = false;
}

bool ChangeSetScheduler::setComplete(const ChangeSet& set) const {
    for (const auto& item : set.target) {
        const auto shown = current.find(item.first);
        if (shown != current.end() && shown->second == item.second) continue;
        const auto found = meshes.find(item.second);
        if (found == meshes.end() || !found->second.built || !found->second.uploaded) return false;
    }
    return true;
}

bool ChangeSetScheduler::pollActivation(Activation& out) {
    if (sets.empty() || !setComplete(sets.front())) return false;
    ChangeSet set = std::move(sets.front());
    sets.erase(sets.begin());
    out = Activation{};
    out.setId = set.id;
    out.wasEditSet = set.isEdit;
    out.changedBounds = set.changed;
    lastActivated = set.id;
    for (const auto& item : current) {
        const auto wanted = set.target.find(item.first);
        if (wanted == set.target.end() || !(wanted->second == item.second)) out.hide.push_back(item.second);
    }
    for (const auto& item : set.target) {
        const auto shown = current.find(item.first);
        if (shown != current.end() && shown->second == item.second) continue;
        const MeshEntry& entry = meshes[item.second];
        out.show.emplace_back(item.second, entry.mesh);
    }
    current = std::move(set.target);
    activations++;
    trimCache();
    startWaitingLod();
    return true;
}

void ChangeSetScheduler::trimCache() {
    if (meshes.size() <= current.size() + options.cacheCapacity) return;
    std::vector<std::pair<uint64_t, MeshId>> candidates;
    const auto wantedBySet = [this](const MeshId& id) {
        for (const ChangeSet& set : sets) {
            const auto found = set.target.find(id.key);
            if (found != set.target.end() && found->second == id) return true;
        }
        return false;
    };
    for (const auto& item : meshes) {
        const auto shown = current.find(item.first.key);
        if (shown != current.end() && shown->second == item.first) continue;
        if (item.second.requested || wantedBySet(item.first)) continue;
        candidates.emplace_back(item.second.lastUse, item.first);
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    size_t excess = meshes.size() - current.size() - options.cacheCapacity;
    for (const auto& candidate : candidates) {
        if (excess == 0) break;
        meshes.erase(candidate.second);
        excess--;
    }
}
