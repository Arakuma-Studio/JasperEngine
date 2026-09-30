// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "BackgroundRelease.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

using namespace jasperengine;

namespace {

    class Releaser {
    public:
        void push(std::shared_ptr<void> item) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!started) {
                    std::thread([this]() { run(); }).detach();
                    started = true;
                }
                queue.push_back(std::move(item));
            }
            wake.notify_one();
        }

    private:
        void run() {
            std::unique_lock<std::mutex> lock(mutex);
            while (true) {
                wake.wait(lock, [this]() { return !queue.empty(); });
                std::deque<std::shared_ptr<void>> batch;
                batch.swap(queue);
                lock.unlock();
                batch.clear(); // the frees happen here, outside the lock
                lock.lock();
            }
        }

        std::mutex mutex;
        std::condition_variable wake;
        std::deque<std::shared_ptr<void>> queue;
        bool started = false;
    };

    // Never destroyed: commands (and what they hold) may be freed during static
    // destruction, after a function-local static would be gone. At exit whatever is
    // still queued is left to the process teardown.
    Releaser& releaser() {
        static Releaser* instance = new Releaser();
        return *instance;
    }

}

void editor::BackgroundRelease::push(std::shared_ptr<HolderBase> holder) {
    releaser().push(std::move(holder));
}
