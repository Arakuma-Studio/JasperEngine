// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "model/FbxImportQueue.h"

#include "Out.h"
#include "jobs/JobSystem.h"

#include <atomic>
#include <chrono>
#include <system_error>
#include <thread>
#include <utility>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {
    std::atomic<int> fbxImportDelayMs{0};
}

void FbxImportQueue::setDelayForTest(int milliseconds) {
    fbxImportDelayMs.store(milliseconds);
}

FbxImportQueue::FbxImportQueue(Hooks hooks): hooks(std::move(hooks)) {
}

FbxImportQueue::~FbxImportQueue() {
    wait();
}

void FbxImportQueue::wait() {
    jobs::Task<void> task;
    {
        std::lock_guard<std::mutex> lock(mutex);
        task = job;
    }
    task.wait();
}

bool FbxImportQueue::stampOf(const fs::path& fbx, Stamp& stamp) {
    std::error_code ec;
    stamp.size = fs::file_size(fbx, ec);
    if (ec) return false;
    stamp.time = fs::last_write_time(fbx, ec);
    return !ec;
}

void FbxImportQueue::enter() {
    if (queued.empty() && running.empty()) {
        counts.batch++;
        counts.done = 0;
        counts.total = 0;
        counts.failed = 0;
        announced = false;
    }
}

void FbxImportQueue::request(const fs::path& fbx, bool force) {
    const fs::path key = fbx.lexically_normal();
    std::lock_guard<std::mutex> lock(mutex);
    // a check of a file converting now would find it current
    if (!force && running.count(key)) return;
    enter();
    auto [it, added] = queued.emplace(key, force);
    it->second = it->second || force;
    // a file asked for again (a cook polling each frame) is no news for start()
    if (added) requests++;
}

bool FbxImportQueue::isPending(const fs::path& fbx) const {
    const fs::path key = fbx.lexically_normal();
    std::lock_guard<std::mutex> lock(mutex);
    return queued.count(key) || running.count(key);
}

bool FbxImportQueue::hasFailed(const fs::path& fbx) const {
    const fs::path key = fbx.lexically_normal();
    Stamp now;
    const bool exists = stampOf(key, now);
    std::lock_guard<std::mutex> lock(mutex);
    auto it = failures.find(key);
    return exists && it != failures.end() && it->second.size == now.size && it->second.time == now.time;
}

FbxImportQueue::Progress FbxImportQueue::progress() const {
    std::lock_guard<std::mutex> lock(mutex);
    Progress progress = counts;
    progress.active = !queued.empty() || !running.empty();
    return progress;
}

std::vector<FbxImportQueue::Item> FbxImportQueue::takeQueued() {
    std::vector<Item> items(queued.begin(), queued.end());
    for (const Item& item : items) running.insert(item.first);
    queued.clear();
    return items;
}

void FbxImportQueue::start() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        const bool settled = requests == requestsSeen;
        requestsSeen = requests;
        if (jobRunning || queued.empty() || !settled) return;
        jobRunning = true;
    }
    // hashing and converting take seconds: a Background job (S06), Heavy (a conversion parses the
    // whole model)
    jobs::SubmitOptions options;
    options.lane = jobs::Lane::Background;
    options.gate = jobs::GateClass::Heavy;
    options.name = "fbx-import";
    jobs::Task<void> task = jobs::submit([this]() { runJob(); }, options);
    std::lock_guard<std::mutex> lock(mutex);
    job = task;
}

void FbxImportQueue::runJob() {
    for (;;) {
        std::vector<Item> items;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (queued.empty()) {
                jobRunning = false;
                break;
            }
            items = takeQueued();
        }
        process(items);
    }
    if (hooks.changed) hooks.changed();
}

bool FbxImportQueue::convertNow(const fs::path& fbx, bool force) {
    const fs::path key = fbx.lexically_normal();
    {
        std::unique_lock<std::mutex> lock(mutex);
        ended.wait(lock, [&]() { return running.count(key) == 0; });
        auto it = queued.find(key);
        if (it != queued.end()) {
            force = force || it->second;
            queued.erase(it);
        } else {
            enter();
        }
        running.insert(key);
    }
    const bool ok = process({{key, force}});
    if (hooks.changed) hooks.changed();
    return ok;
}

void FbxImportQueue::drain() {
    for (;;) {
        std::vector<Item> items;
        {
            std::unique_lock<std::mutex> lock(mutex);
            if (queued.empty()) {
                ended.wait(lock, [&]() { return !queued.empty() || running.empty(); });
                if (queued.empty()) break;
            }
            items = takeQueued();
        }
        process(items);
    }
    if (hooks.changed) hooks.changed();
}

bool FbxImportQueue::process(const std::vector<Item>& items) {
    // what each file needs first, so the batch's line counts them
    std::vector<Item> work;
    size_t created = 0;
    bool ok = true;
    for (const auto& [fbx, force] : items) {
        // a failed file is tried again once it changes
        if (!force && hasFailed(fbx)) {
            ok = false;
            finish(fbx, false, false);
            continue;
        }
        Need need = hooks.need(fbx);
        if (force && need == Need::None) need = Need::Update;
        if (need == Need::None) {
            finish(fbx, false, true);
            continue;
        }
        if (need == Need::Create) created++;
        work.emplace_back(fbx, force);
    }
    if (!work.empty()) {
        std::lock_guard<std::mutex> lock(mutex);
        counts.total += work.size();
        // models a scene shows only once their first import lands: said once per batch (REQ-33)
        if (created > 0 && !announced) {
            announced = true;
            Out::info("Importing %zu model%s (first open): %s when ready", created, created == 1 ? "" : "s",
                      created == 1 ? "it appears" : "they appear");
        }
    }
    for (const auto& [fbx, force] : work) {
        if (const int delay = fbxImportDelayMs.load(); delay > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
        const bool converted = hooks.convert(fbx, force);
        ok = ok && converted;
        finish(fbx, true, converted);
        if (hooks.changed) hooks.changed();
    }
    return ok;
}

void FbxImportQueue::finish(const fs::path& fbx, bool converted, bool ok) {
    Stamp stamp;
    const bool stamped = !ok && converted && stampOf(fbx, stamp);
    {
        std::lock_guard<std::mutex> lock(mutex);
        running.erase(fbx);
        if (converted) {
            counts.done++;
            if (ok) {
                failures.erase(fbx);
            } else {
                counts.failed++;
                if (stamped) failures[fbx] = stamp;
            }
        }
    }
    ended.notify_all();
}
