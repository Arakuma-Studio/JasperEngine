//
// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
// Guard execution of untrusted/user code to prevent process termination on crashes.
// POSIX: catches SIGSEGV/SIGBUS/SIGFPE/SIGILL/SIGABRT via sigaction + sigsetjmp/siglongjmp
// Windows: uses SEH (__try/__except) to catch access violations.
//
// NOTE: Recovering from fatal signals/exceptions skips C++ destructors on the stack frames
// between the fault point and the guard boundary. Only use this around untrusted callbacks.
//

#pragma once

#include <functional>
#include "diag/CrashHandler.h"

// IMPORTANT: Include platform headers at global scope (not inside any namespace)
// to avoid injecting standard names inside jasperengine::std, which breaks builds.
#if defined(_WIN32) && defined(_MSC_VER)
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <windows.h>
#elif defined(__EMSCRIPTEN__)
// no special headers
#elif !defined(_WIN32)
    // Use C headers for POSIX signal and setjmp APIs to get C-level names.
    #include <signal.h>
    #include <setjmp.h>
    #include <cstring>
#endif

namespace jasperengine {

    struct CrashInfo {
        int code;                // signal number or Windows exception code
        const char* name;        // short name
        const char* description; // human-friendly description
    };

#if defined(_WIN32) && defined(_MSC_VER)

    inline const char* winExceptionName(DWORD code) {
        switch (code) {
            case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
            case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
            case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
            case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
            case EXCEPTION_FLT_INVALID_OPERATION: return "EXCEPTION_FLT_INVALID_OPERATION";
            case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
            case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
            case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
            case 0xE06D7363: return "CXX_EXCEPTION";
            default: return "EXCEPTION_UNKNOWN";
        }
    }

    inline const char* winExceptionDesc(DWORD code) {
        switch (code) {
            case EXCEPTION_ACCESS_VIOLATION: return "Access violation (likely null or invalid pointer dereference)";
            case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "Array bounds exceeded";
            case EXCEPTION_DATATYPE_MISALIGNMENT: return "Datatype misalignment";
            case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "Floating-point divide by zero";
            case EXCEPTION_FLT_INVALID_OPERATION: return "Invalid floating-point operation";
            case EXCEPTION_INT_DIVIDE_BY_ZERO: return "Integer divide by zero";
            case EXCEPTION_ILLEGAL_INSTRUCTION: return "Illegal instruction";
            case EXCEPTION_STACK_OVERFLOW: return "Stack overflow";
            // MSVC's code of a C++ throw (CrashHook::runGuarded catches them before they get here)
            case 0xE06D7363: return "C++ exception";
            default: return "Unknown structured exception";
        }
    }

    template<typename Fn>
    inline bool callWithCrashGuard(Fn&& fn, CrashInfo* outInfo) {
        bool ok = false;
        __try {
            fn();
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            if (outInfo) {
                DWORD code = GetExceptionCode();
                outInfo->code = static_cast<int>(code);
                outInfo->name = winExceptionName(code);
                outInfo->description = winExceptionDesc(code);
            }
            ok = false;
        }
        return ok;
    }

#elif defined(__EMSCRIPTEN__)

    // Emscripten/WASM does not support POSIX signals or SEH. Just execute directly.
    template<typename Fn>
    inline bool callWithCrashGuard(Fn&& fn, CrashInfo* /*outInfo*/) {
        fn();
        return true;
    }

#elif !defined(_WIN32)
    // POSIX implementation

    namespace detail {
        struct GuardTLS {
            sigjmp_buf env;
            bool active = false;
            int lastSig = 0;
        };

        // One TLS state per thread
        inline thread_local GuardTLS g_tls;

        inline const char* sigName(int sig) {
            switch (sig) {
                case SIGSEGV: return "SIGSEGV";
                case SIGBUS:  return "SIGBUS";
                case SIGFPE:  return "SIGFPE";
                case SIGILL:  return "SIGILL";
                case SIGABRT: return "SIGABRT";
                default:      return "SIGNAL";
            }
        }

        inline const char* sigDesc(int sig) {
            switch (sig) {
                case SIGSEGV: return "Segmentation fault (invalid memory reference)";
                case SIGBUS:  return "Bus error (misaligned or invalid access)";
                case SIGFPE:  return "Floating-point exception";
                case SIGILL:  return "Illegal instruction";
                case SIGABRT: return "Abort signal";
                default:      return "Fatal signal";
            }
        }

        inline void crashSignalHandler(int sig, siginfo_t* info, void* ucontext) {
            if (g_tls.active) {
                g_tls.lastSig = sig;
                siglongjmp(g_tls.env, 1);
            } else {
                // Another thread can fault while a Lua callback holds this process-wide guard.
                diag::crash::handleFatalSignal(sig, info, ucontext);
            }
        }

        struct SignalScope {
            struct sigaction old_segv{};
            struct sigaction old_bus{};
            struct sigaction old_fpe{};
            struct sigaction old_ill{};
            struct sigaction old_abrt{};
            bool installed = false;

            void install() {
                struct sigaction sa{};
                sa.sa_sigaction = &crashSignalHandler;
                sigemptyset(&sa.sa_mask);
                sigaddset(&sa.sa_mask, SIGSEGV);
                sigaddset(&sa.sa_mask, SIGBUS);
                sigaddset(&sa.sa_mask, SIGFPE);
                sigaddset(&sa.sa_mask, SIGILL);
                sigaddset(&sa.sa_mask, SIGABRT);
                sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;

                sigaction(SIGSEGV, &sa, &old_segv);
                sigaction(SIGBUS,  &sa, &old_bus);
                sigaction(SIGFPE,  &sa, &old_fpe);
                sigaction(SIGILL,  &sa, &old_ill);
                sigaction(SIGABRT, &sa, &old_abrt);
                installed = true;
            }

            void restore() {
                if (!installed) return;
                sigaction(SIGSEGV, &old_segv, nullptr);
                sigaction(SIGBUS,  &old_bus,  nullptr);
                sigaction(SIGFPE,  &old_fpe,  nullptr);
                sigaction(SIGILL,  &old_ill,  nullptr);
                sigaction(SIGABRT, &old_abrt, nullptr);
                installed = false;
            }
        };
    } // namespace detail

    namespace detail {
        // One guard's frame: the thread's previous guard state (an outer guard's jump buffer, a
        // nesting guard returning hands it back) and the signal handlers before this guard. Put
        // back when the guard ends however it ends: a normal return, a recovered fault, or a C++
        // exception a direct caller's fn threw through it.
        struct GuardFrame {
            SignalScope scope;
            sigjmp_buf previousEnv;
            bool previousActive;
            int previousSig;

            GuardFrame() : previousActive(g_tls.active), previousSig(g_tls.lastSig) {
                std::memcpy(&previousEnv, &g_tls.env, sizeof(sigjmp_buf));
                scope.install();
            }

            ~GuardFrame() {
                std::memcpy(&g_tls.env, &previousEnv, sizeof(sigjmp_buf));
                g_tls.active = previousActive;
                g_tls.lastSig = previousSig;
                scope.restore();
            }

            GuardFrame(const GuardFrame&) = delete;
            GuardFrame& operator=(const GuardFrame&) = delete;
        };
    } // namespace detail

    template<typename Fn>
    inline bool callWithCrashGuard(Fn&& fn, CrashInfo* outInfo) {
        using namespace detail;

        GuardFrame frame;

        g_tls.active = true;
        g_tls.lastSig = 0;

        // sigsetjmp stays in this function's own frame: the jump lands here
        if (sigsetjmp(g_tls.env, 1) == 0) {
            // Execute the user function
            fn();
            return true;
        }

        diag::crash::ringWrite("CrashGuard: recovered a guarded callback crash");
        // We jumped back from a signal
        if (outInfo) {
            outInfo->code = g_tls.lastSig;
            outInfo->name = sigName(g_tls.lastSig);
            outInfo->description = sigDesc(g_tls.lastSig);
        }
        return false;
    }

#else
    // Windows with non-MSVC compilers (e.g., MinGW/clang-mingw):
    // SEH __try/__except is not available, and POSIX sigaction isn't reliable on Windows.
    // Fall back to direct execution (no crash guard).
    template<typename Fn>
    inline bool callWithCrashGuard(Fn&& fn, CrashInfo* /*outInfo*/) {
        fn();
        return true;
    }
#endif

} // namespace jasperengine