// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "GpuMemory.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>

using namespace jasperengine;
using namespace jasperengine::gpumem;

namespace {

    const char* const CATEGORY_NAMES[CATEGORY_COUNT] = {
        "Textures", "Render Targets", "Shadow Maps", "Post FX", "Meshes (VB/IB)", "Uniform/Streaming",
        "UI", "Particles", "Terrain", "Editor", "Other"
    };
    const char* const CATEGORY_KEYS[CATEGORY_COUNT] = {
        "textures", "render_targets", "shadow_maps", "post_fx", "meshes", "uniforms",
        "ui", "particles", "terrain", "editor", "other"
    };

}

const char* gpumem::categoryName(Category category){
    const size_t index = (size_t)category;
    return index < CATEGORY_COUNT ? CATEGORY_NAMES[index] : "Other";
}

const char* gpumem::categoryKey(Category category){
    const size_t index = (size_t)category;
    return index < CATEGORY_COUNT ? CATEGORY_KEYS[index] : "other";
}

bool gpumem::categoryFromKey(std::string_view key, Category& out){
    for (size_t i = 0; i < CATEGORY_COUNT; i++){
        if (key == CATEGORY_KEYS[i] || key == CATEGORY_NAMES[i]){
            out = (Category)i;
            return true;
        }
    }
    return false;
}

const char* gpumem::kindName(Kind kind){
    switch (kind){
        case Kind::Texture: return "Texture";
        case Kind::RenderTarget: return "RenderTarget";
        case Kind::DepthTarget: return "DepthTarget";
        case Kind::Buffer: return "Buffer";
    }
    return "Texture";
}

#if JASPERENGINE_PROFILER

namespace {

    struct Tracker {
        std::mutex mutex;
        std::vector<std::string> owners{std::string()};   // id 0: none
        std::unordered_map<std::string, uint32_t> ownerIds;
        std::unordered_map<uint32_t, Entry> images;
        std::unordered_map<uint32_t, Entry> buffers;
        uint64_t totalBytes = 0;
        uint64_t peakBytes = 0;
        uint64_t generation = 0;
        DriverQuery driver = nullptr;
    };

    Tracker& tracker(){
        static Tracker* t = new Tracker();   // outlives the static destructors that free resources
        return *t;
    }

    thread_local ScopeState threadScope;

}

uint32_t gpumem::internOwner(std::string_view owner){
    if (owner.empty())
        return 0;
    Tracker& t = tracker();
    std::lock_guard<std::mutex> lock(t.mutex);
    auto it = t.ownerIds.find(std::string(owner));
    if (it != t.ownerIds.end())
        return it->second;
    const uint32_t id = (uint32_t)t.owners.size();
    t.owners.emplace_back(owner);
    t.ownerIds.emplace(t.owners.back(), id);
    return id;
}

std::string gpumem::ownerName(uint32_t owner){
    Tracker& t = tracker();
    std::lock_guard<std::mutex> lock(t.mutex);
    return owner < t.owners.size() ? t.owners[owner] : std::string();
}

ScopeState gpumem::currentScope(){
    return threadScope;
}

Scope::Scope(Category category, const char* owner): Scope(category, std::string_view(owner ? owner : "")){
}

Scope::Scope(Category category, std::string_view owner): saved(threadScope){
    threadScope.category = (uint8_t)category;
    threadScope.owner = internOwner(owner);
}

Scope::Scope(ScopeState state): saved(threadScope){
    threadScope = state;
}

Scope::~Scope(){
    threadScope = saved;
}

void gpumem::track(const Entry& entry, bool image){
    Tracker& t = tracker();
    std::lock_guard<std::mutex> lock(t.mutex);
    auto& table = image ? t.images : t.buffers;
    auto it = table.find(entry.id);
    if (it != table.end())
        t.totalBytes -= it->second.bytes;
    table[entry.id] = entry;
    t.totalBytes += entry.bytes;
    t.peakBytes = std::max(t.peakBytes, t.totalBytes);
    t.generation++;
}

void gpumem::untrack(uint32_t id, bool image){
    Tracker& t = tracker();
    std::lock_guard<std::mutex> lock(t.mutex);
    auto& table = image ? t.images : t.buffers;
    auto it = table.find(id);
    if (it == table.end())
        return;
    t.totalBytes -= it->second.bytes;
    table.erase(it);
    t.generation++;
}

const char* gpumem::label(uint32_t id, bool image){
    Tracker& t = tracker();
    std::lock_guard<std::mutex> lock(t.mutex);
    auto& table = image ? t.images : t.buffers;
    auto it = table.find(id);
    // the entry stays put until it is untracked (main thread, as the caller)
    return it != table.end() ? it->second.label : "";
}

void gpumem::clearAll(){
    Tracker& t = tracker();
    std::lock_guard<std::mutex> lock(t.mutex);
    t.images.clear();
    t.buffers.clear();
    t.totalBytes = 0;
    t.peakBytes = 0;
    t.generation++;
}

void gpumem::resetPeak(){
    Tracker& t = tracker();
    std::lock_guard<std::mutex> lock(t.mutex);
    t.peakBytes = t.totalBytes;
}

uint64_t gpumem::generation(){
    Tracker& t = tracker();
    std::lock_guard<std::mutex> lock(t.mutex);
    return t.generation;
}

void gpumem::setDriverQuery(DriverQuery query){
    Tracker& t = tracker();
    std::lock_guard<std::mutex> lock(t.mutex);
    t.driver = query;
}

Report gpumem::report(bool withAllocations){
    Report out;
    DriverQuery driver = nullptr;
    {
        Tracker& t = tracker();
        std::lock_guard<std::mutex> lock(t.mutex);
        out.totalBytes = t.totalBytes;
        out.peakBytes = t.peakBytes;
        driver = t.driver;
        auto add = [&](const Entry& entry, bool image){
            const size_t category = std::min((size_t)entry.category, CATEGORY_COUNT - 1);
            out.categoryBytes[category] += entry.bytes;
            out.categoryCount[category]++;
            out.count++;
            if (!entry.tagged)
                out.untaggedBytes += entry.bytes;
            if (!withAllocations)
                return;
            Allocation allocation;
            allocation.id = entry.id;
            allocation.image = image;
            allocation.kind = entry.kind;
            allocation.category = entry.category;
            allocation.tagged = entry.tagged;
            allocation.owner = entry.owner < t.owners.size() ? t.owners[entry.owner] : std::string();
            allocation.label = entry.label;
            if (allocation.owner.empty())
                allocation.owner = allocation.label.empty() ? std::string("(unnamed)") : allocation.label;
            allocation.width = entry.width;
            allocation.height = entry.height;
            allocation.slices = entry.slices;
            allocation.mips = entry.mips;
            allocation.samples = entry.samples;
            allocation.format = entry.format ? entry.format : "";
            allocation.usage = entry.usage;
            allocation.bytes = entry.bytes;
            out.allocations.push_back(std::move(allocation));
        };
        for (const auto& [id, entry] : t.images)
            add(entry, true);
        for (const auto& [id, entry] : t.buffers)
            add(entry, false);
    }
    std::sort(out.allocations.begin(), out.allocations.end(), [](const Allocation& a, const Allocation& b){
        return a.bytes != b.bytes ? a.bytes > b.bytes : a.id < b.id;
    });
    if (driver)
        out.driver = driver();
    return out;
}

std::vector<Group> gpumem::groupByOwner(const Report& report){
    std::unordered_map<std::string, Group> groups;
    for (const Allocation& allocation : report.allocations){
        Group& group = groups[allocation.owner];
        group.name = allocation.owner;
        group.bytes += allocation.bytes;
        group.count++;
    }
    std::vector<Group> out;
    out.reserve(groups.size());
    for (auto& [name, group] : groups)
        out.push_back(std::move(group));
    std::sort(out.begin(), out.end(), [](const Group& a, const Group& b){
        return a.bytes != b.bytes ? a.bytes > b.bytes : a.name < b.name;
    });
    return out;
}

#endif
