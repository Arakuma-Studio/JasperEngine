// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef threadpoolmanager_h
#define threadpoolmanager_h

#include "Export.h"
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <functional>
#include <atomic>
#include <memory>
#include <vector>

namespace jasperengine {

    class JASPERENGINE_API ThreadPoolManager {
    private:
        static std::unique_ptr<ThreadPoolManager> instance;
        static std::mutex instanceMutex;
        
        std::vector<std::thread> workers;
        std::queue<std::function<void()>> tasks;
        mutable std::mutex queueMutex;
        std::condition_variable condition;
        std::atomic<bool> stop{false};
        
        ThreadPoolManager(size_t numThreads = std::thread::hardware_concurrency());
        
    public:
        static ThreadPoolManager& getInstance();
        static void initialize(size_t maxThreads = std::thread::hardware_concurrency());
        static void shutdown();
        
        template<class F, class... Args>
        auto enqueue(F&& f, Args&&... args) 
            -> std::future<std::invoke_result_t<F, Args...>>;
            
        size_t getQueueSize() const;
        ~ThreadPoolManager();
    };

    template<class F, class... Args>
    auto ThreadPoolManager::enqueue(F&& f, Args&&... args) 
        -> std::future<std::invoke_result_t<F, Args...>> {
        
        using return_type = std::invoke_result_t<F, Args...>;
        
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        
        std::future<return_type> res = task->get_future();
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            
            if(stop)
                throw std::runtime_error("enqueue on stopped ThreadPool");
                
            tasks.emplace([task](){ (*task)(); });
        }
        condition.notify_one();
        return res;
    }

}

#endif /* threadpoolmanager_h */
