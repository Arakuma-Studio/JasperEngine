// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Memory module (Modules/Memory/Include/Memory/AddressSanitizer.h,
// SerialUseCheck.h), Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT
// License. See NOTICE.md.

#ifndef MEMORY_MEMORYDEBUG_H
#define MEMORY_MEMORYDEBUG_H

#include <atomic>
#include <cassert>
#include <cstdio>

// JE_MEMORY_ASAN: the translation unit is compiled with AddressSanitizer (clang's
// __has_feature, or GCC's and MSVC's __SANITIZE_ADDRESS__). The allocators then poison the
// memory they hold but have not handed out.
#if defined(__SANITIZE_ADDRESS__)
    #define JE_MEMORY_ASAN 1
#elif defined(__has_feature)
    #if __has_feature(address_sanitizer)
        #define JE_MEMORY_ASAN 1
    #endif
#endif
#if defined(JE_MEMORY_ASAN)
    #include <sanitizer/asan_interface.h>
    #define JE_MEMORY_POISON(address, bytes) ASAN_POISON_MEMORY_REGION((address), (bytes))
    #define JE_MEMORY_UNPOISON(address, bytes) ASAN_UNPOISON_MEMORY_REGION((address), (bytes))
#else
    #define JE_MEMORY_POISON(address, bytes) ((void)(address), (void)(bytes))
    #define JE_MEMORY_UNPOISON(address, bytes) ((void)(address), (void)(bytes))
#endif

// JE_MEMORY_CHECKS: builds with assertions fill fresh memory with 0xCD and released memory
// with 0xDD, and check serial use, double deletes and foreign pointers.
#ifndef JE_MEMORY_CHECKS
    #ifdef NDEBUG
        #define JE_MEMORY_CHECKS 0
    #else
        #define JE_MEMORY_CHECKS 1
    #endif
#endif


namespace jasperengine { namespace memory { namespace detail {

    // Checks that an externally serialized allocator has one user at a time: each state
    // change marks the word busy for its duration and asserts it was free. It records no
    // thread, so uses on different threads ordered by a join or a lock pass.
    class SerialUseCheck {
    public:
        void enter(const char* allocator){
            if (busy.exchange(true, std::memory_order_acquire)){
                std::fprintf(stderr, "two threads used this %s at once; it is externally serialized: one user at a "
                                     "time (give each thread its own, or order the uses with a join or a lock)\n", allocator);
                assert(false && "two threads used one externally serialized allocator at once");
            }
        }
        void exit(){ busy.store(false, std::memory_order_release); }

    private:
        std::atomic<bool> busy{false};
    };

    class SerialUseScope {
    public:
#if JE_MEMORY_CHECKS
        SerialUseScope(SerialUseCheck& check, const char* allocator): check(check){ check.enter(allocator); }
        ~SerialUseScope(){ check.exit(); }
#else
        SerialUseScope(SerialUseCheck&, const char*) {}
#endif
        SerialUseScope(const SerialUseScope&) = delete;
        SerialUseScope& operator=(const SerialUseScope&) = delete;

    private:
#if JE_MEMORY_CHECKS
        SerialUseCheck& check;
#endif
    };

}}}

#endif // MEMORY_MEMORYDEBUG_H
