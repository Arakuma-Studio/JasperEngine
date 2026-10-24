// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ThreadUtil.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#elif defined(__EMSCRIPTEN__)
    #include <emscripten.h>
    #include <emscripten/threading.h>
    #include <pthread.h>
#else
    #include <pthread.h>
    #include <unistd.h>
    #if defined(__APPLE__)
        #include <TargetConditionals.h>
        #include <pthread/qos.h>
        #include <sys/sysctl.h>
    #else
        #include <sys/resource.h>
        #include <sys/syscall.h>
    #endif
#endif

namespace jasperengine { namespace thread {

PlatformClass currentPlatform(){
#if defined(__EMSCRIPTEN__)
    #if defined(__EMSCRIPTEN_PTHREADS__)
        return PlatformClass::WebThreads;
    #else
        return PlatformClass::WebSingle;
    #endif
#elif defined(__ANDROID__)
    return PlatformClass::Android;
#elif defined(__APPLE__) && TARGET_OS_IPHONE
    return PlatformClass::IOS;
#else
    return PlatformClass::Desktop;
#endif
}

int detectCoreCount(){
    int cores = 0;
#if defined(__EMSCRIPTEN__)
    #if defined(__EMSCRIPTEN_PTHREADS__)
        cores = emscripten_num_logical_cores();
    #else
        cores = 1;
    #endif
#elif defined(__ANDROID__)
    // CONF, not ONLN: a big core that is hot-unplugged when the call runs still counts
    cores = (int)sysconf(_SC_NPROCESSORS_CONF);
#elif defined(__APPLE__) && TARGET_OS_IPHONE
    int value = 0;
    size_t size = sizeof(value);
    if (sysctlbyname("hw.logicalcpu", &value, &size, nullptr, 0) == 0)
        cores = value;
#else
    cores = (int)std::thread::hardware_concurrency();
#endif
    return cores > 0 ? cores : 1;
}

int webThreadPoolSize(){
#if defined(__EMSCRIPTEN__) && defined(__EMSCRIPTEN_PTHREADS__)
    #if !defined(JE_PTHREAD_POOL_SIZE)
        #error "a threaded web build defines JE_PTHREAD_POOL_SIZE, the -sPTHREAD_POOL_SIZE it links with (engine/CMakeLists.txt)"
    #endif
    return JE_PTHREAD_POOL_SIZE;
#else
    return 0;
#endif
}

int webWorkerBudget(int poolSize){
    return std::max(1, poolSize - kWebPoolThreadsBesideWorkers);
}

int workerCountFor(PlatformClass platform, int cores, int webPoolSize){
    switch (platform){
        case PlatformClass::Desktop:
            return std::clamp(cores - 1, 1, 15);
        case PlatformClass::Android:
        case PlatformClass::IOS:
            return std::clamp(cores - 1, 1, 4);
        case PlatformClass::WebThreads:
            // the pool of the build bounds them: a thread asked beyond it aborts (STRICT=2)
            return std::clamp(cores - 2, 1, std::min(4, webWorkerBudget(webPoolSize)));
        case PlatformClass::WebSingle:
            return 0;
    }
    return 1;
}

int defaultWorkerCount(){
    return workerCountFor(currentPlatform(), detectCoreCount());
}

int resolveWorkerCount(int configured){
    const PlatformClass platform = currentPlatform();
    if (platform == PlatformClass::WebSingle)
        return 0;
    // a setting or JE_JOB_WORKERS asks; the platform caps
    const int cap = platform == PlatformClass::WebThreads ? webWorkerBudget(webThreadPoolSize()) : 15;
    if (configured >= 0)
        return std::min(configured, cap);
    if (const char* env = std::getenv("JE_JOB_WORKERS")){
        if (env[0] != '\0'){
            char* end = nullptr;
            const long value = std::strtol(env, &end, 10);
            if (end != env && value >= 0)
                return (int)std::min<long>(value, cap);
        }
    }
    return defaultWorkerCount();
}

void setCurrentName(const char* name){
    if (!name)
        return;
#if defined(_WIN32)
    // SetThreadDescription exists from Windows 10 1607; looked up so older systems still run
    typedef HRESULT (WINAPI *SetThreadDescriptionFn)(HANDLE, PCWSTR);
    static const SetThreadDescriptionFn setDescription = [](){
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        return kernel ? (SetThreadDescriptionFn)(void*)GetProcAddress(kernel, "SetThreadDescription") : nullptr;
    }();
    if (setDescription){
        wchar_t wide[64];
        size_t i = 0;
        for (; name[i] != '\0' && i < 63; i++)
            wide[i] = (wchar_t)(unsigned char)name[i];
        wide[i] = L'\0';
        setDescription(GetCurrentThread(), wide);
    }
#elif defined(__EMSCRIPTEN__)
    // emscripten_set_thread_name links only with -sPTHREADS_PROFILING: web threads keep no name
    (void)name;
#elif defined(__APPLE__)
    pthread_setname_np(name);
#else
    // Linux and Android cap the name at 15 characters and fail rather than truncate
    char shortName[16];
    std::strncpy(shortName, name, sizeof(shortName) - 1);
    shortName[sizeof(shortName) - 1] = '\0';
    pthread_setname_np(pthread_self(), shortName);
#endif
}

void setCurrentPriority(Priority priority){
#if defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), priority == Priority::BelowNormal ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL);
#elif defined(__EMSCRIPTEN__)
    (void)priority;
#elif defined(__APPLE__)
    pthread_set_qos_class_self_np(priority == Priority::BelowNormal ? QOS_CLASS_UTILITY : QOS_CLASS_USER_INITIATED, 0);
#else
    if (priority == Priority::BelowNormal){
        // per thread on Linux: setpriority on a thread id changes only that thread
        const pid_t tid = (pid_t)syscall(SYS_gettid);
        const int processNice = getpriority(PRIO_PROCESS, 0);
        setpriority(PRIO_PROCESS, (id_t)tid, std::min(processNice + 5, 19));
    }
#endif
}

bool canBlock(){
#if defined(__EMSCRIPTEN__)
    #if defined(__EMSCRIPTEN_PTHREADS__)
        return !emscripten_is_main_browser_thread();
    #else
        return false;
    #endif
#else
    return true;
#endif
}

uint32_t currentThreadIndex(){
    static std::atomic<uint32_t> next{1};
    thread_local const uint32_t index = next.fetch_add(1, std::memory_order_relaxed);
    return index;
}

std::thread spawnNamed(const char* name, std::function<void()> fn, Priority priority){
    std::string threadName = name ? name : "je-thread";
    return std::thread([threadName = std::move(threadName), fn = std::move(fn), priority](){
        setCurrentName(threadName.c_str());
        setCurrentPriority(priority);
        fn();
    });
}

}}
