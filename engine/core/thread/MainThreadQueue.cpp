// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MainThreadQueue.h"

#include "Log.h"

#include <atomic>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>

using namespace jasperengine;

namespace {

    struct QueueState {
        std::mutex mutex;
        std::deque<std::function<void()>> tasks;
        std::atomic<uint64_t> epoch{0};
        std::atomic<std::thread::id> mainThread{};
    };

    // never destroyed: a worker may post while the process exits
    QueueState& queueState(){
        static QueueState* state = new QueueState();
        return *state;
    }

}

void MainThreadQueue::post(std::function<void()> task){
    if (!task) return;
    QueueState& state = queueState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.tasks.push_back(std::move(task));
}

void MainThreadQueue::drain(){
    QueueState& state = queueState();
    if (state.mainThread.load(std::memory_order_relaxed) != std::this_thread::get_id()){
        state.mainThread.store(std::this_thread::get_id(), std::memory_order_relaxed);
    }

    std::deque<std::function<void()>> tasks;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.tasks.empty()) return;
        tasks.swap(state.tasks);
    }
    const uint64_t epoch = state.epoch.load(std::memory_order_acquire);
    while (!tasks.empty()){
        // a task that called clear() dropped the ones after it too
        if (state.epoch.load(std::memory_order_acquire) != epoch) break;
        std::function<void()> task = std::move(tasks.front());
        tasks.pop_front();
        try {
            task();
        } catch (const std::exception& e) {
            Log::error("A main-thread task failed: %s", e.what());
        } catch (...) {
            Log::error("A main-thread task failed with an unknown exception");
        }
    }
}

size_t MainThreadQueue::pending(){
    QueueState& state = queueState();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.tasks.size();
}

void MainThreadQueue::clear(){
    QueueState& state = queueState();
    std::deque<std::function<void()>> dropped;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        dropped.swap(state.tasks);
        state.epoch.fetch_add(1, std::memory_order_acq_rel);
    }
    // destroyed here, outside the lock: a task's captures may post (or clear) again
}

uint64_t MainThreadQueue::epoch(){
    return queueState().epoch.load(std::memory_order_acquire);
}

bool MainThreadQueue::isMainThread(){
    const std::thread::id main = queueState().mainThread.load(std::memory_order_relaxed);
    return main == std::thread::id() || main == std::this_thread::get_id();
}

void MainThreadQueue::setMainThread(){
    queueState().mainThread.store(std::this_thread::get_id(), std::memory_order_relaxed);
}
