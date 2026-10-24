// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Timed condition-variable waits for the job system, the asset readers and the primitives.
// libstdc++ waits on the steady clock with pthread_cond_clockwait, which GCC 11's
// ThreadSanitizer does not intercept: it misses the unlock inside the wait and reports double
// locks and races that cannot happen. TSan builds wait on the system clock instead
// (pthread_cond_timedwait, intercepted); a clock step there only shortens or stretches one
// backstop wait.

#ifndef JOBS_DETAIL_TIMEDWAIT_H
#define JOBS_DETAIL_TIMEDWAIT_H

#include <chrono>
#include <condition_variable>
#include <mutex>

#if defined(__SANITIZE_THREAD__)
#define JE_TIMED_WAIT_SYSTEM_CLOCK 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define JE_TIMED_WAIT_SYSTEM_CLOCK 1
#endif
#endif

namespace jasperengine::jobs::detail {

    template<class Rep, class Period>
    inline void timedWait(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                          const std::chrono::duration<Rep, Period>& timeout){
#ifdef JE_TIMED_WAIT_SYSTEM_CLOCK
        cv.wait_until(lock, std::chrono::system_clock::now() + timeout);
#else
        cv.wait_for(lock, timeout);
#endif
    }

    // Returns the predicate's final value, like condition_variable::wait_for.
    template<class Rep, class Period, class Predicate>
    inline bool timedWait(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                          const std::chrono::duration<Rep, Period>& timeout, Predicate predicate){
#ifdef JE_TIMED_WAIT_SYSTEM_CLOCK
        return cv.wait_until(lock, std::chrono::system_clock::now() + timeout, predicate);
#else
        return cv.wait_for(lock, timeout, predicate);
#endif
    }

}

#endif //JOBS_DETAIL_TIMEDWAIT_H
