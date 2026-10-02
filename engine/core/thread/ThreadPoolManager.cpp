// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ThreadPoolManager.h"

using namespace jasperengine;

std::unique_ptr<ThreadPoolManager> ThreadPoolManager::instance = nullptr;
std::mutex ThreadPoolManager::instanceMutex;

ThreadPoolManager::ThreadPoolManager(size_t numThreads) {
    for(size_t i = 0; i < numThreads; ++i) {
        workers.emplace_back([this] {
            for(;;) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(this->queueMutex);
                    this->condition.wait(lock, [this]{ 
                        return this->stop || !this->tasks.empty(); 
                    });
                    
                    if(this->stop && this->tasks.empty())
                        return;
                        
                    task = std::move(this->tasks.front());
                    this->tasks.pop();
                }
                task();
            }
        });
    }
}

ThreadPoolManager& ThreadPoolManager::getInstance() {
    std::lock_guard<std::mutex> lock(instanceMutex);
    if (!instance) {
        instance = std::unique_ptr<ThreadPoolManager>(new ThreadPoolManager());
    }
    return *instance;
}

void ThreadPoolManager::initialize(size_t maxThreads) {
    std::lock_guard<std::mutex> lock(instanceMutex);
    if (!instance) {
        instance = std::unique_ptr<ThreadPoolManager>(new ThreadPoolManager(maxThreads));
    }
}

size_t ThreadPoolManager::getQueueSize() const {
    std::lock_guard<std::mutex> lock(queueMutex);
    return tasks.size();
}

void ThreadPoolManager::shutdown() {
    std::lock_guard<std::mutex> lock(instanceMutex);
    instance.reset();
}

ThreadPoolManager::~ThreadPoolManager() {
    stop = true;
    condition.notify_all();
    for(std::thread &worker: workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}
