// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VoxelWorkQueue.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <chrono>
#include <utility>

using namespace jasperengine;
using namespace jasperengine::voxel;

namespace {
    // The lane's own average would lag by a window: a slow block should reduce the next frame's slice
    constexpr double AVERAGE_SMOOTHING = 0.2;
}

WorkQueue::WorkQueue(jobs::JobSystem* system): requested(system), sharedAverage(std::make_shared<std::atomic<float>>(1.0f)) {}

WorkQueue::~WorkQueue() {
    items.clear();
    for (Running& item : running) {
        item.task.cancel();
        item.task.wait();
    }
}

namespace {

    std::mutex voxelFinishedNotifierMutex;
    std::function<void()> voxelFinishedNotifier;

    void notifyVoxelJobFinished() {
        std::function<void()> notify;
        {
            std::lock_guard<std::mutex> lock(voxelFinishedNotifierMutex);
            notify = voxelFinishedNotifier;
        }
        if (notify) notify();
    }

}

void WorkQueue::setFinishedNotifier(std::function<void()> notify) {
    std::lock_guard<std::mutex> lock(voxelFinishedNotifierMutex);
    voxelFinishedNotifier = std::move(notify);
}

void WorkQueue::push(uint64_t id, WorkPriority priority, double distance, std::function<void()> work) {
    Item item;
    item.id = id;
    item.priority = priority;
    item.distance = distance;
    item.work = std::move(work);
    items.push_back(std::move(item));
}

bool WorkQueue::cancel(uint64_t id) {
    for (size_t i = 0; i < items.size(); i++) {
        if (items[i].id == id) {
            items.erase(items.begin() + static_cast<std::ptrdiff_t>(i));
            droppedIds.push_back(id);
            return true;
        }
    }
    for (Running& item : running) {
        if (item.id == id) return item.task.cancel();
    }
    return false;
}

void WorkQueue::cancelAndWait(uint64_t id) {
    if (cancel(id)) return;
    for (Running& item : running) {
        if (item.id == id) item.task.wait();
    }
}

void WorkQueue::reprioritize(const Rank& rank) {
    for (Item& item : items) rank(item.id, item.priority, item.distance);
}

size_t WorkQueue::nextIndex() const {
    size_t best = 0;
    for (size_t i = 1; i < items.size(); i++) {
        const Item& a = items[i];
        const Item& b = items[best];
        if (a.priority != b.priority) {
            if (a.priority < b.priority) best = i;
        } else if (a.distance != b.distance) {
            if (a.distance < b.distance) best = i;
        } else if (a.id < b.id) {
            best = i;
        }
    }
    return best;
}

int WorkQueue::slotsToFill() const {
    jobs::JobSystem* system = requested ? requested : (jobs::hasSystem() ? &jobs::system() : nullptr);
    if (!system) return static_cast<int>(items.size());
    const int workers = system->getWorkerCount();
    if (workers > 0) {
        const int limit = std::max(1, workers - 1);
        return std::max(0, limit - static_cast<int>(running.size()));
    }
    // 0 workers: the pump's budget, respected to within one job (R15)
    const double perFrame = pumpBudgetMs / std::max(averageMs, 0.001);
    return std::max(0, std::clamp(static_cast<int>(perFrame), 1, 4) - static_cast<int>(running.size()));
}

void WorkQueue::pollRunning() {
    for (size_t i = 0; i < running.size();) {
        if (!running[i].task.isReady()) {
            i++;
            continue;
        }
        if (running[i].task.status() == jobs::TaskStatus::Completed) {
            finishedIds.push_back(running[i].id);
        } else {
            droppedIds.push_back(running[i].id);
        }
        running.erase(running.begin() + static_cast<std::ptrdiff_t>(i));
    }
    averageMs = sharedAverage->load(std::memory_order_relaxed);
}

void WorkQueue::submitNext() {
    const size_t index = nextIndex();
    Item item = std::move(items[index]);
    items.erase(items.begin() + static_cast<std::ptrdiff_t>(index));
    jobs::JobSystem* system = requested ? requested : (jobs::hasSystem() ? &jobs::system() : nullptr);
    if (!system) {
        item.work();
        finishedIds.push_back(item.id);
        return;
    }
    std::shared_ptr<std::atomic<float>> average = sharedAverage;
    auto work = std::move(item.work);
    jobs::SubmitOptions options;
    options.lane = jobs::Lane::Background;
    options.gate = jobs::GateClass::Default;
    options.name = "voxel-work";
    Running entry;
    entry.id = item.id;
    entry.task = system->submit([work = std::move(work), average]() {
        const auto start = std::chrono::steady_clock::now();
        work();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        // Only a pacing estimate: two jobs finishing together may lose one sample, which costs nothing
        const double previous = average->load(std::memory_order_relaxed);
        average->store(static_cast<float>(previous + (ms - previous) * AVERAGE_SMOOTHING), std::memory_order_relaxed);
        notifyVoxelJobFinished();
        return true;
    }, options);
    running.push_back(std::move(entry));
    peak = std::max(peak, running.size());
}

void WorkQueue::pump() {
    pollRunning();
    int slots = slotsToFill();
    while (slots > 0 && !items.empty()) {
        submitNext();
        slots--;
    }
}

void WorkQueue::takeFinished(std::vector<uint64_t>& finished, std::vector<uint64_t>& dropped) {
    finished.swap(finishedIds);
    finishedIds.clear();
    dropped.swap(droppedIds);
    droppedIds.clear();
}

void WorkQueue::finishAll() {
    while (!items.empty() || !running.empty()) {
        pump();
        for (Running& item : running) item.task.wait();
        pollRunning();
    }
}
