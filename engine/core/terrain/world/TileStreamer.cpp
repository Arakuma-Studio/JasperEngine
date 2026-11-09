// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "terrain/world/TileStreamer.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::terrain;

namespace {

    // R14: the backoff before retry 1, 2 and 3
    constexpr double RETRY_DELAYS[TileStreamer::MAX_RETRIES] = {0.5, 1.0, 2.0};

}

const char* jasperengine::terrain::tileStateName(TileState state) {
    switch (state) {
        case TileState::Absent: return "absent";
        case TileState::Cold: return "cold";
        case TileState::Queued: return "queued";
        case TileState::Loading: return "loading";
        case TileState::CpuReady: return "cpu_ready";
        case TileState::Uploading: return "uploading";
        case TileState::Resident: return "resident";
        case TileState::Error: return "error";
    }
    return "absent";
}

TileStreamer::TileStreamer(TileIO& io, const WorldOccupancy& occupancy): io(io), occupancy(occupancy) {}

TileStreamer::Record* TileStreamer::find(TileCoord tile) {
    const auto it = records.find(tile.key());
    return it == records.end() ? nullptr : &it->second;
}

const TileStreamer::Record* TileStreamer::find(TileCoord tile) const {
    const auto it = records.find(tile.key());
    return it == records.end() ? nullptr : &it->second;
}

void TileStreamer::setFocuses(const std::vector<StreamFocus>& focuses) {
    focusList = focuses;
    if (focusList.size() > static_cast<size_t>(WORLD_MAX_FOCUSES)) focusList.resize(WORLD_MAX_FOCUSES);
    focusChanged = true;
}

void TileStreamer::demand(TileCoord tile, TilePriority priority) {
    for (auto& [coord, level] : demands) {
        if (coord == tile) {
            level = std::max(level, priority);
            return;
        }
    }
    demands.emplace_back(tile, priority);
    focusChanged = true;
}

void TileStreamer::pin(TileCoord tile, bool pinned) {
    if (pinned) {
        records[tile.key()].pinned = true;
    } else if (Record* record = find(tile)) {
        record->pinned = false;
    }
}

bool TileStreamer::pinned(TileCoord tile) const {
    const Record* record = find(tile);
    return record && record->pinned;
}

float TileStreamer::bandRadius(const StreamFocus& focus) const {
    // the near band: where a tile node draws from its page (D4's r_T)
    return 2.0f * worldLattice.tileSize() * limits.lodDistance * focus.radiusScale;
}

float TileStreamer::distanceToTile(float x, float z, TileCoord tile) const {
    const float size = worldLattice.tileSize();
    const float minX = static_cast<float>(tile.x) * size, minZ = static_cast<float>(tile.z) * size;
    const float dx = x < minX ? minX - x : (x > minX + size ? x - minX - size : 0.0f);
    const float dz = z < minZ ? minZ - z : (z > minZ + size ? z - minZ - size : 0.0f);
    return std::sqrt(dx * dx + dz * dz);
}

void TileStreamer::want(TileCoord tile, TilePriority priority, float distance, bool focusTile, double now) {
    if (!occupancy.present(tile)) return;
    Record& record = records[tile.key()];
    if (!record.wanted || priority > record.priority || (priority == record.priority && distance < record.distance)) {
        record.priority = record.wanted ? std::max(record.priority, priority) : priority;
        record.distance = record.wanted ? std::min(record.distance, distance) : distance;
    }
    record.wanted = true;
    record.focusTile = record.focusTile || focusTile;
    record.lastWanted = now;
}

void TileStreamer::resort(double now) {
    for (auto& [key, record] : records) {
        record.wanted = false;
        record.focusTile = false;
    }
    const float size = worldLattice.tileSize();
    for (const StreamFocus& focus : focusList) {
        const float radius = bandRadius(focus);
        const float predictedX = focus.x + focus.velocityX * static_cast<float>(PREDICT_SECONDS);
        const float predictedZ = focus.z + focus.velocityZ * static_cast<float>(PREDICT_SECONDS);
        for (int pass = 0; pass < 2; pass++) {
            const float cx = pass == 0 ? focus.x : predictedX, cz = pass == 0 ? focus.z : predictedZ;
            if (pass == 1 && focus.velocityX == 0.0f && focus.velocityZ == 0.0f) break;
            const int reach = static_cast<int>(std::ceil(radius / size)) + 1;
            const int centreX = tileOfLocal(cx, size), centreZ = tileOfLocal(cz, size);
            for (int z = centreZ - reach; z <= centreZ + reach; z++) {
                for (int x = centreX - reach; x <= centreX + reach; x++) {
                    const TileCoord tile(x, z);
                    if (!tile.addressable()) continue;
                    const float distance = distanceToTile(cx, cz, tile);
                    if (distance > radius) continue;
                    const bool own = x == centreX && z == centreZ && pass == 0;
                    want(tile, own ? TilePriority::Critical : (pass == 0 ? TilePriority::High : TilePriority::Normal),
                         distanceToTile(focus.x, focus.z, tile), own, now);
                }
            }
        }
    }
    for (const auto& [tile, priority] : demands) want(tile, priority, 0.0f, priority == TilePriority::Critical, now);
    demands.clear();
    lastResort = now;
    focusChanged = false;
}

void TileStreamer::pollLoads(double now) {
    for (auto& [key, record] : records) {
        if (record.state != TileState::Loading) continue;
        std::shared_ptr<WorldTile> data;
        std::string error;
        const TileLoadStatus status = io.poll(record.handle, data, error);
        if (status == TileLoadStatus::Pending) continue;
        record.handle = 0;
        const TileCoord tile = TileCoord::fromKey(key);
        if (status == TileLoadStatus::Done && data) {
            record.cpuBytes = data->cpuBytes();
            record.data = std::move(data);
            record.state = TileState::CpuReady;
            record.failures = 0;
            counters.loaded++;
            events.push_back(TileStreamEvent{TileStreamEvent::Kind::CpuReady, tile, ""});
            continue;
        }
        record.failures++;
        if (record.failures > MAX_RETRIES) {
            record.state = TileState::Error;
            if (!record.errorLogged) {
                record.errorLogged = true;
                events.push_back(TileStreamEvent{TileStreamEvent::Kind::Error, tile, error});
            }
        } else {
            record.state = TileState::Cold;
            record.retryAt = now + RETRY_DELAYS[record.failures - 1];
        }
    }
}

void TileStreamer::cancelFarLoads() {
    const float size = worldLattice.tileSize();
    for (auto& [key, record] : records) {
        if (record.state != TileState::Loading && record.state != TileState::Queued) continue;
        if (record.wanted) continue;
        const TileCoord tile = TileCoord::fromKey(key);
        bool near = false;
        for (const StreamFocus& focus : focusList) {
            const float radius = bandRadius(focus) + 2.0f * size;
            const float px = focus.x + focus.velocityX * static_cast<float>(PREDICT_SECONDS);
            const float pz = focus.z + focus.velocityZ * static_cast<float>(PREDICT_SECONDS);
            near = near || distanceToTile(focus.x, focus.z, tile) <= radius || distanceToTile(px, pz, tile) <= radius;
        }
        if (near) continue;
        if (record.state == TileState::Loading) io.cancel(record.handle);
        record.handle = 0;
        record.state = TileState::Cold;
        counters.cancelled++;
        events.push_back(TileStreamEvent{TileStreamEvent::Kind::Cancelled, tile, ""});
    }
}

void TileStreamer::startLoads(double now) {
    struct Candidate {
        uint32_t key;
        TilePriority priority;
        float distance;
    };
    std::vector<Candidate> queue;
    int inFlight = 0;
    for (auto& [key, record] : records) {
        if (record.state == TileState::Loading) inFlight++;
        if (record.state == TileState::Queued) record.state = TileState::Cold;
        if (record.state == TileState::Cold && record.wanted && now >= record.retryAt) {
            queue.push_back(Candidate{key, record.priority, record.distance});
        }
    }
    std::sort(queue.begin(), queue.end(), [](const Candidate& a, const Candidate& b) {
        if (a.priority != b.priority) return a.priority > b.priority;
        if (a.distance != b.distance) return a.distance < b.distance;
        return a.key < b.key;
    });
    for (const Candidate& candidate : queue) {
        Record& record = records[candidate.key];
        if (inFlight >= limits.maxInFlight) {
            record.state = TileState::Queued;
            continue;
        }
        record.handle = io.begin(TileCoord::fromKey(candidate.key), candidate.priority);
        record.state = TileState::Loading;
        inFlight++;
    }
}

void TileStreamer::evict() {
    // least recently wanted first; never the focus's tile, a pinned one or one wanted now
    auto victims = [&](bool pages) {
        std::vector<std::pair<double, uint32_t>> list;
        for (const auto& [key, record] : records) {
            if (record.pinned || record.focusTile || record.wanted) continue;
            if (pages ? record.page : (record.data && !record.page)) list.emplace_back(record.lastWanted, key);
        }
        std::sort(list.begin(), list.end());
        return list;
    };
    recount();
    // make room for the wanted tiles waiting for a page, as well as staying under the caps
    int waiting = 0;
    uint64_t waitingBytes = 0;
    for (const auto& [key, record] : records) {
        if (record.wanted && !record.page && record.data && (record.state == TileState::CpuReady || record.state == TileState::Uploading)) {
            waiting++;
            waitingBytes += pageBytes(*record.data);
        }
    }
    const int targetPages = std::max(0, limits.poolSize - waiting);
    const uint64_t targetBytes = limits.gpuCapBytes > waitingBytes ? limits.gpuCapBytes - waitingBytes : 0;
    if (counters.residentPages > targetPages || counters.gpuBytes > targetBytes) {
        for (const auto& [age, key] : victims(true)) {
            if (counters.residentPages <= targetPages && counters.gpuBytes <= targetBytes) break;
            Record& record = records[key];
            record.page = false;
            record.stale = false;
            counters.residentPages--;
            counters.gpuBytes -= record.gpuBytes;
            record.gpuBytes = 0;
            record.state = record.data ? TileState::CpuReady : TileState::Cold;
            counters.evictedPages++;
            events.push_back(TileStreamEvent{TileStreamEvent::Kind::PageEvicted, TileCoord::fromKey(key), ""});
        }
    }
    // the pool shrank under the pages it holds: the farthest wanted ones go too, never the focus's
    // tile or a pinned one (R8; the overview draws them, R34)
    if (counters.residentPages > limits.poolSize) {
        std::vector<std::pair<float, uint32_t>> wantedPages;
        for (const auto& [key, record] : records) {
            if (record.page && record.wanted && !record.pinned && !record.focusTile) wantedPages.emplace_back(record.distance, key);
        }
        std::sort(wantedPages.begin(), wantedPages.end(), [](const auto& a, const auto& b) {
            return a.first != b.first ? a.first > b.first : a.second > b.second;
        });
        for (const auto& [distance, key] : wantedPages) {
            if (counters.residentPages <= limits.poolSize) break;
            Record& record = records[key];
            record.page = false;
            record.stale = false;
            counters.residentPages--;
            counters.gpuBytes -= record.gpuBytes;
            record.gpuBytes = 0;
            record.state = record.data ? TileState::CpuReady : TileState::Cold;
            counters.evictedPages++;
            events.push_back(TileStreamEvent{TileStreamEvent::Kind::PageEvicted, TileCoord::fromKey(key), ""});
        }
    }
    if (counters.cpuBytes > limits.cpuCapBytes) {
        for (const auto& [age, key] : victims(false)) {
            if (counters.cpuBytes <= limits.cpuCapBytes) break;
            Record& record = records[key];
            counters.cpuBytes -= record.cpuBytes;
            record.cpuBytes = 0;
            record.data.reset();
            record.state = TileState::Cold;
            counters.evictedCpu++;
            events.push_back(TileStreamEvent{TileStreamEvent::Kind::CpuEvicted, TileCoord::fromKey(key), ""});
        }
    }
}

void TileStreamer::recount() {
    for (int& count : counters.states) count = 0;
    counters.inFlight = 0;
    counters.queued = 0;
    counters.cpuBytes = 0;
    counters.gpuBytes = 0;
    counters.residentPages = 0;
    counters.errors = 0;
    for (const auto& [key, record] : records) {
        counters.states[static_cast<int>(record.state)]++;
        if (record.state == TileState::Loading) counters.inFlight++;
        if (record.state == TileState::Queued) counters.queued++;
        if (record.state == TileState::Error) counters.errors++;
        counters.cpuBytes += record.cpuBytes;
        counters.gpuBytes += record.gpuBytes;
        if (record.page) counters.residentPages++;
    }
    // tiles with no record are present and cold
    counters.states[static_cast<int>(TileState::Cold)] += std::max(0, occupancy.tileCount() - static_cast<int>(records.size()));
}

void TileStreamer::update(double now) {
    pollLoads(now);
    if (isFrozen) {
        recount();
        return;
    }
    if (focusChanged || now - lastResort >= RESORT_SECONDS) {
        resort(now);
        cancelFarLoads();
    }
    startLoads(now);
    evict();
    recount();
}

uint64_t TileStreamer::pageBytes(const WorldTile& tile) const {
    // height RG8 with gutter, the blend maps and the hole map as the page holds them (R34)
    uint64_t bytes = static_cast<uint64_t>(tile.height.width) * tile.height.height * 2;
    for (const TileMap& map : tile.blend) bytes += map.bytes.size();
    bytes += tile.holes.bytes.size();
    return bytes;
}

std::vector<TileCoord> TileStreamer::planUploads() {
    counters.uploadBytesLastFrame = 0;
    counters.poolExhausted = 0;
    std::vector<TileCoord> plan;
    if (isFrozen || !limits.gpu) return plan;
    struct Candidate {
        uint32_t key;
        TilePriority priority;
        float distance;
        bool stale;
    };
    std::vector<Candidate> list;
    for (const auto& [key, record] : records) {
        const bool fresh = (record.state == TileState::CpuReady || record.state == TileState::Uploading) && record.wanted && record.data;
        const bool stale = record.page && record.stale && record.data;
        if (fresh || stale) list.push_back(Candidate{key, record.priority, record.distance, stale});
    }
    std::sort(list.begin(), list.end(), [](const Candidate& a, const Candidate& b) {
        if (a.priority != b.priority) return a.priority > b.priority;
        if (a.distance != b.distance) return a.distance < b.distance;
        return a.key < b.key;
    });
    int pages = counters.residentPages;
    uint64_t gpu = counters.gpuBytes;
    uint64_t bytes = 0;
    for (const Candidate& candidate : list) {
        Record& record = records[candidate.key];
        const uint64_t cost = pageBytes(*record.data);
        if (!candidate.stale && (pages >= limits.poolSize || gpu + cost > limits.gpuCapBytes)) {
            counters.poolExhausted++;
            continue;
        }
        if (!plan.empty() && bytes + cost > limits.uploadBytesPerFrame) break;
        bytes += cost;
        plan.push_back(TileCoord::fromKey(candidate.key));
        if (!candidate.stale) {
            pages++;
            gpu += cost;
            record.state = TileState::Uploading;
        }
    }
    counters.uploadBytesLastFrame = bytes;
    counters.uploadBytesTotal += bytes;
    return plan;
}

void TileStreamer::uploaded(TileCoord tile, uint64_t gpuBytes) {
    Record* record = find(tile);
    if (!record || !record->data) return;
    if (record->page) {
        counters.gpuBytes -= record->gpuBytes;
    } else {
        counters.residentPages++;
    }
    counters.gpuBytes += gpuBytes;
    record->gpuBytes = gpuBytes;
    record->page = true;
    record->stale = false;
    record->state = TileState::Resident;
}

void TileStreamer::uploadRefused(TileCoord tile) {
    counters.uploadRetries++;
    if (Record* record = find(tile)) {
        if (!record->page) record->state = TileState::CpuReady;
    }
}

TileState TileStreamer::state(TileCoord tile) const {
    if (!occupancy.present(tile)) return TileState::Absent;
    const Record* record = find(tile);
    return record ? record->state : TileState::Cold;
}

std::shared_ptr<const WorldTile> TileStreamer::data(TileCoord tile) const {
    const Record* record = find(tile);
    return record ? record->data : nullptr;
}

bool TileStreamer::wanted(TileCoord tile) const {
    const Record* record = find(tile);
    return record && record->wanted;
}

bool TileStreamer::pageStale(TileCoord tile) const {
    const Record* record = find(tile);
    return record && record->stale;
}

void TileStreamer::replace(TileCoord tile, std::shared_ptr<const WorldTile> data) {
    Record& record = records[tile.key()];
    if (record.state == TileState::Loading) io.cancel(record.handle);
    record.handle = 0;
    record.cpuBytes = data ? data->cpuBytes() : 0;
    record.data = std::move(data);
    record.failures = 0;
    record.errorLogged = false;
    if (record.page) {
        record.stale = true;
        record.state = TileState::Resident;
    } else {
        record.state = record.data ? TileState::CpuReady : TileState::Cold;
    }
}

void TileStreamer::invalidate(TileCoord tile) {
    Record* record = find(tile);
    if (!record) return;
    if (record->state == TileState::Loading) io.cancel(record->handle);
    record->handle = 0;
    record->failures = 0;
    record->errorLogged = false;
    record->retryAt = 0.0;
    record->data.reset();
    record->cpuBytes = 0;
    if (record->page) {
        // the old page draws until the reloaded one replaces it
        record->stale = true;
    }
    record->state = TileState::Cold;
    focusChanged = true;
}

void TileStreamer::forget(TileCoord tile) {
    Record* record = find(tile);
    if (!record) return;
    if (record->state == TileState::Loading) io.cancel(record->handle);
    if (record->page) events.push_back(TileStreamEvent{TileStreamEvent::Kind::PageEvicted, tile, ""});
    records.erase(tile.key());
}

std::vector<TileStreamEvent> TileStreamer::takeEvents() {
    std::vector<TileStreamEvent> out = std::move(events);
    events.clear();
    return out;
}

std::vector<TileCoord> TileStreamer::knownTiles() const {
    std::vector<TileCoord> out;
    out.reserve(records.size());
    for (const auto& [key, record] : records) out.push_back(TileCoord::fromKey(key));
    return out;
}
