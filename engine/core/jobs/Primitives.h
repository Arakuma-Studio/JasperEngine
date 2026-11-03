// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JOBS_PRIMITIVES_H
#define JOBS_PRIMITIVES_H

// Parking primitives that work at every platform floor: mutex + condition variable only.
// std::atomic::wait, std::counting_semaphore, std::latch and std::stop_token are gated by
// libc++ on the macOS/iOS versions the engine still supports, so engine code uses these.

#include "Export.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace jasperengine { namespace jobs {

    // A manual-reset event: set() releases every waiter until reset().
    class JASPERENGINE_API Event {
    public:
        void set();
        void reset();
        bool isSet() const;
        void wait();
        // False on timeout.
        bool waitFor(double milliseconds);

    private:
        mutable std::mutex mutex;
        std::condition_variable cv;
        bool signalled = false;
    };

    // A counting semaphore.
    class JASPERENGINE_API Semaphore {
    public:
        explicit Semaphore(uint32_t initial = 0);
        void release(uint32_t count = 1);
        void acquire();
        bool tryAcquire();
        // False on timeout.
        bool tryAcquireFor(double milliseconds);

    private:
        std::mutex mutex;
        std::condition_variable cv;
        uint32_t count;
    };

    namespace detail {
        struct CancelState {
            std::atomic<uint32_t> refs{1};
            std::atomic<bool> cancelled{false};
        };
        JASPERENGINE_API void retainCancel(CancelState* state);
        JASPERENGINE_API void releaseCancel(CancelState* state);
    }

    // A cooperative cancellation flag shared by copies: cancelling one cancels every copy,
    // so one token can cover a group of jobs (a load = read + decode + upload). Pass it in
    // SubmitOptions::cancel; a running job polls jobs::cancelRequested(). Comparison is by
    // identity (the shared flag), which is what AssetIOService::cancel matches on.
    class JASPERENGINE_API CancelToken {
    public:
        CancelToken();
        CancelToken(const CancelToken& other);
        CancelToken(CancelToken&& other) noexcept;
        CancelToken& operator=(const CancelToken& other);
        CancelToken& operator=(CancelToken&& other) noexcept;
        ~CancelToken();

        void cancel();
        bool isCancelled() const;

        bool operator==(const CancelToken& other) const { return state == other.state; }
        bool operator!=(const CancelToken& other) const { return state != other.state; }

        detail::CancelState* getState() const { return state; }

    private:
        detail::CancelState* state;
    };

}}

#endif // JOBS_PRIMITIVES_H
