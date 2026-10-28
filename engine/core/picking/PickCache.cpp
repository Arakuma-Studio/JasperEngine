// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's MeshBvhCache (Apps/Editor/Source/Picking/MeshBvhCache.cpp),
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License (NOTICE.md).

#include "PickCache.h"

#include "jobs/JobSystem.h"
#include "profiler/Profile.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace jasperengine;

std::atomic<uint32_t> PickCache::maxTriangles{2000000u};

PickCache& PickCache::get(){
    // never destroyed: build jobs and scene teardown at exit may still release into it, after
    // the job system's statics are gone
    static PickCache* cache = new PickCache();
    return *cache;
}

PickCache::~PickCache(){
    // a build job refers to its cache: none may outlive it
    std::vector<jobs::TaskRef> tasks;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& entry : entries){
            if (entry.second->task){
                tasks.push_back(entry.second->task);
            }
        }
    }
    for (jobs::TaskRef& task : tasks){
        task.cancel();
        task.wait();
    }
    std::lock_guard<std::mutex> lock(mutex);
    entries.clear();
}

void PickCache::setMaxTriangles(uint32_t triangles){
    maxTriangles.store(triangles, std::memory_order_relaxed);
}

uint32_t PickCache::getMaxTriangles(){
    return maxTriangles.load(std::memory_order_relaxed);
}

void PickCache::setDiskCache(PickDiskCache* cache){
    disk.store(cache, std::memory_order_release);
}

PickDiskCache* PickCache::getDiskCache() const{
    return disk.load(std::memory_order_acquire);
}

namespace {

    int backgroundBuildDelayMs(){
        static const int delay = [](){
            const char* value = std::getenv("JASPER_PICK_BUILD_DELAY_MS");
            return value ? std::max(0, std::atoi(value)) : 0;
        }();
        return delay;
    }

}

PickCache::EntryPtr PickCache::acquire(uint64_t hash){
    std::lock_guard<std::mutex> lock(mutex);
    auto it = entries.find(hash);
    if (it == entries.end()){
        return nullptr;
    }
    it->second->users++;
    return it->second;
}

PickCache::EntryPtr PickCache::create(uint64_t hash, PickGeometry&& geometry, bool inlineBuild){
    EntryPtr entry;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = entries.find(hash);
        if (it != entries.end()){
            it->second->users++;
            return it->second;
        }
        entry = std::make_shared<Entry>();
        entry->hash = hash;
        entry->triangles = geometry.getTriangleCount();
        entry->geometry = std::make_shared<PickGeometry>(std::move(geometry));
        entry->users = 1;
        entries.emplace(hash, entry);

        const auto now = std::chrono::steady_clock::now();
        if (now - lastCollect > std::chrono::seconds(1)){
            lastCollect = now;
            collectLocked(now);
        }
    }

    if (inlineBuild){
        runBuild(entry);
    }else{
        jobs::SubmitOptions options;
        options.lane = jobs::Lane::Background;
        options.gate = entry->triangles >= DISK_CACHE_TRIANGLES ? jobs::GateClass::Heavy : jobs::GateClass::Default;
        options.name = "mesh-bvh";
        std::weak_ptr<Entry> weak = entry;
        entry->task = jobs::submit([this, weak](){
            // tests: JASPER_PICK_BUILD_DELAY_MS keeps a background build pending that long, to
            // see the Scene view's waiting click and refused drop (docs/testing.md)
            if (const int delay = backgroundBuildDelayMs(); delay > 0){
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            }
            if (EntryPtr owned = weak.lock()){
                runBuild(owned);
            }
        }, options);
    }
    return entry;
}

void PickCache::runBuild(const EntryPtr& entry){
    std::lock_guard<std::mutex> buildLock(entry->buildMutex);
    if (entry->ready.load(std::memory_order_acquire) || !entry->geometry){
        return;
    }
    JE_PROFILE_SCOPE("MeshBvh build");

    const auto start = std::chrono::steady_clock::now();
    auto bvh = std::make_shared<MeshBvh>();
    bool fromDisk = false;
    PickDiskCache* diskCache = getDiskCache();
    if (diskCache && entry->triangles >= DISK_CACHE_TRIANGLES && diskCache->load(entry->hash, *entry->geometry, *bvh)){
        fromDisk = true;
    }else{
        *bvh = MeshBvh::build(*entry->geometry);
        if (diskCache && entry->triangles >= DISK_CACHE_TRIANGLES){
            diskCache->store(entry->hash, *bvh);
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

    {
        std::lock_guard<std::mutex> lock(mutex);
        entry->buildMs = ms;
        entry->bytes = bvh->memoryBytes();
        entry->fromDisk = fromDisk;
        entry->bvh = std::move(bvh);
        entry->geometry.reset();
        buildMsTotal += ms;
    }
    (fromDisk ? diskLoads : builds).fetch_add(1, std::memory_order_relaxed);
    entry->ready.store(true, std::memory_order_release);
}

void PickCache::release(const EntryPtr& entry){
    if (!entry){
        return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (entry->users > 0){
        entry->users--;
        if (entry->users == 0){
            entry->released = std::chrono::steady_clock::now();
        }
    }
}

const MeshBvh* PickCache::ready(const EntryPtr& entry){
    if (!entry || !entry->ready.load(std::memory_order_acquire)){
        return nullptr;
    }
    return entry->bvh.get();
}

const MeshBvh* PickCache::wait(const EntryPtr& entry){
    if (!entry){
        return nullptr;
    }
    if (const MeshBvh* bvh = ready(entry)){
        return bvh;
    }
    inlineWaits.fetch_add(1, std::memory_order_relaxed);
    if (entry->task){
        // runs the job here when no worker took it yet (and single-threaded)
        entry->task.wait();
    }
    if (!entry->ready.load(std::memory_order_acquire)){
        // cancelled (shutdown) or never queued: built here
        runBuild(entry);
    }
    return ready(entry);
}

void PickCache::collectLocked(std::chrono::steady_clock::time_point now){
    const auto keep = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(DROP_AFTER_SECONDS));
    for (auto it = entries.begin(); it != entries.end();){
        const Entry& entry = *it->second;
        // a pending build stays until it finished, so its job never builds for nobody twice
        if (entry.users == 0 && entry.ready.load(std::memory_order_acquire) && now - entry.released > keep){
            it = entries.erase(it);
        }else{
            ++it;
        }
    }
}

void PickCache::collect(std::chrono::steady_clock::time_point now){
    std::lock_guard<std::mutex> lock(mutex);
    lastCollect = now;
    collectLocked(now);
}

void PickCache::clear(){
    std::vector<EntryPtr> pending;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& entry : entries){
            if (entry.second->task && !entry.second->task.isReady()){
                pending.push_back(entry.second);
            }
        }
        entries.clear();
    }
    // queued builds are not left running for entries nobody can find
    for (const EntryPtr& entry : pending){
        entry->task.cancel();
    }
}

PickCacheStats PickCache::stats() const{
    PickCacheStats result;
    std::lock_guard<std::mutex> lock(mutex);
    result.entries = entries.size();
    for (const auto& entry : entries){
        if (entry.second->ready.load(std::memory_order_acquire)){
            result.built++;
            result.bytes += entry.second->bytes;
        }else{
            result.pending++;
        }
    }
    result.buildMsTotal = buildMsTotal;
    result.builds = builds.load(std::memory_order_relaxed);
    result.diskLoads = diskLoads.load(std::memory_order_relaxed);
    result.inlineWaits = inlineWaits.load(std::memory_order_relaxed);
    return result;
}
