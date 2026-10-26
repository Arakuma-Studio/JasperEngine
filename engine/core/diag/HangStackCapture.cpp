// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HangWatchdog.h"
#include "detail/CrashState.h"
#include "log/Backtrace.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#elif !defined(__EMSCRIPTEN__)
#include <csignal>
#include <cstdlib>
#include <pthread.h>
#include <time.h>
#endif

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
namespace je_crash_posix { size_t capture(void** frames, size_t capacity); }
#endif
namespace je_hang_capture {
using namespace jasperengine;
using namespace diag::crash::detail;
constexpr size_t maxFrames = 64;
void* frames[maxFrames]{};
size_t frameCount = 0;
bool available = false;
#if defined(_WIN32)
HANDLE mainThread = nullptr;
DWORD mainThreadId = 0;
HANDLE requestEvent = nullptr;
HANDLE readyEvent = nullptr;
CONTEXT capturedContext{};
std::atomic<bool> disabled{false};
std::atomic<bool> stallWalker{false};
char capturedText[16384]{};
size_t capturedTextBytes = 0;

DWORD machineFrame(CONTEXT& context, STACKFRAME64& frame) {
    frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
#if defined(_M_X64) || defined(__x86_64__)
    frame.AddrPC.Offset = context.Rip;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrFrame.Offset = context.Rbp;
    return IMAGE_FILE_MACHINE_AMD64;
#elif defined(_M_ARM64) || defined(__aarch64__)
    frame.AddrPC.Offset = context.Pc;
    frame.AddrStack.Offset = context.Sp;
    frame.AddrFrame.Offset = context.Fp;
    return IMAGE_FILE_MACHINE_ARM64;
#else
    frame.AddrPC.Offset = context.Eip;
    frame.AddrStack.Offset = context.Esp;
    frame.AddrFrame.Offset = context.Ebp;
    return IMAGE_FILE_MACHINE_I386;
#endif
}
// Run symbol APIs only in the helper: even after ResumeThread, DbgHelp itself can
// block on a loader lock. The watchdog's deadline must include symbol formatting.
void formatCapturedFrames(bool symbols) {
    capturedTextBytes = 0;
    for (size_t i = 0; i < frameCount; i++) {
        const DWORD64 pc = reinterpret_cast<uintptr_t>(frames[i]);
        MEMORY_BASIC_INFORMATION memory{};
        const uintptr_t base = VirtualQuery(frames[i], &memory, sizeof(memory))
            ? reinterpret_cast<uintptr_t>(memory.AllocationBase) : 0;
        IMAGEHLP_MODULE64 module{};
        module.SizeOfStruct = sizeof(module);
        const char* name = nullptr;
        if (symbols && SymGetModuleInfo64(static_cast<HANDLE>(Backtrace::dbgHelpProcess()), pc, &module)) {
            name = module.ImageName;
            for (const char* c = name; *c; c++) if (*c == '/' || *c == '\\') name = c + 1;
        }
        size_t n = capturedTextBytes;
        n = appendText(capturedText, sizeof(capturedText), n, "#");
        n = appendDecimal(capturedText, sizeof(capturedText), n, i);
        n = appendText(capturedText, sizeof(capturedText), n, " ");
        if (name && *name) n = appendText(capturedText, sizeof(capturedText), n, name);
        else {
            n = appendText(capturedText, sizeof(capturedText), n, "module@");
            n = appendHex(capturedText, sizeof(capturedText), n, base);
        }
        n = appendText(capturedText, sizeof(capturedText), n, "+");
        n = appendHex(capturedText, sizeof(capturedText), n, pc - base);
        if (symbols) {
            alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256]{};
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            DWORD64 displacement = 0;
            if (SymFromAddr(static_cast<HANDLE>(Backtrace::dbgHelpProcess()), pc, &displacement, symbol)) {
                n = appendText(capturedText, sizeof(capturedText), n, " [");
                n = appendText(capturedText, sizeof(capturedText), n, symbol->Name);
                n = appendText(capturedText, sizeof(capturedText), n, "+");
                n = appendHex(capturedText, sizeof(capturedText), n, displacement);
                n = appendText(capturedText, sizeof(capturedText), n, "]");
            }
        }
        capturedTextBytes = appendText(capturedText, sizeof(capturedText), n, "\n");
    }
}
DWORD WINAPI walkMain(void*) {
    while (WaitForSingleObject(requestEvent, INFINITE) == WAIT_OBJECT_0) {
        if (stallWalker.load(std::memory_order_relaxed)) Sleep(1000);
        // DbgHelp may wait on a lock the suspended main thread owns. Only this helper can
        // block; the watchdog that resumes the main thread never calls DbgHelp.
        std::unique_lock<std::timed_mutex> lock(Backtrace::dbgHelpMutex(), std::defer_lock);
        frameCount = 0;
        if (lock.try_lock_for(std::chrono::milliseconds(200)) && Backtrace::initializeSymbolsLocked()) {
            CONTEXT context = capturedContext;
            STACKFRAME64 frame{};
            const DWORD machine = machineFrame(context, frame);
            if (frame.AddrPC.Offset) frames[frameCount++] = reinterpret_cast<void*>(frame.AddrPC.Offset);
            while (frameCount < maxFrames && StackWalk64(machine, static_cast<HANDLE>(Backtrace::dbgHelpProcess()), mainThread, &frame, &context,
                    nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) && frame.AddrPC.Offset) {
                frames[frameCount++] = reinterpret_cast<void*>(frame.AddrPC.Offset);
            }
            formatCapturedFrames(true);
        } else {
            capturedTextBytes = 0;
        }
        SetEvent(readyEvent);
    }
    return 0;
}
#elif !defined(__EMSCRIPTEN__)
pthread_t mainThread{};
int captureSignal = 0;
// 0 idle, 1 requested, 2 handler writing, 3 ready. Timeout disables capture for this session:
// the static storage remains alive even if a late signal/handler has not completed yet.
std::atomic<int> requestState{0};
std::atomic<bool> disabled{false};
struct sigaction oldAction{};
bool signalInstalled = false;
void onCapture(int, siginfo_t*, void*) {
    if (!pthread_equal(pthread_self(), mainThread)) return;
    int expected = 1;
    if (!requestState.compare_exchange_strong(expected, 2, std::memory_order_acq_rel)) return;
    frameCount = je_crash_posix::capture(frames, maxFrames);
    requestState.store(3, std::memory_order_release);
}
#endif
}

namespace jasperengine::diag::watchdog::detail {
void initializeMainThreadCapture() {
    using namespace je_hang_capture;
    available = false;
#if defined(_WIN32)
    if (disabled.load(std::memory_order_acquire)) return;
    if (mainThread) CloseHandle(mainThread);
    mainThread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
    mainThreadId = GetCurrentThreadId();
    if (!mainThread) return;
    if (!requestEvent) {
        requestEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        readyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!requestEvent || !readyEvent) return;
        // Every Sym* call (including startup) shares the logger's process-wide lock.
        {
            std::lock_guard<std::timed_mutex> lock(Backtrace::dbgHelpMutex());
            Backtrace::initializeSymbolsLocked();
        }
        HANDLE helper = CreateThread(nullptr, 0, walkMain, nullptr, 0, nullptr);
        if (!helper) return;
        CloseHandle(helper); // lifetime is the process's; no join can hold up shutdown
    }
    available = true;
#elif !defined(__EMSCRIPTEN__)
    // A timed-out handler can still be inside the unwinder; never reuse its buffer.
    if (disabled.load(std::memory_order_acquire)) return;
    mainThread = pthread_self();
    void* warm[16];
    je_crash_posix::capture(warm, 16);
    if (!signalInstalled) {
#if defined(__APPLE__)
        captureSignal = SIGUSR2; // Darwin has no realtime signals
#else
        captureSignal = SIGRTMIN + 5;
#endif
        if (const char* value = std::getenv("JASPERENGINE_HANG_SIGNAL")) {
            char* end = nullptr;
            const long signal = std::strtol(value, &end, 10);
            if (end == value || *end || signal <= 0 || signal >= NSIG) return;
            captureSignal = static_cast<int>(signal);
        }
        if (captureSignal == SIGSEGV || captureSignal == SIGBUS || captureSignal == SIGABRT ||
            captureSignal == SIGFPE || captureSignal == SIGILL || captureSignal == SIGTRAP ||
            captureSignal == SIGKILL || captureSignal == SIGSTOP) return;
        if (sigaction(captureSignal, nullptr, &oldAction) != 0) return;
        // Do not steal a library's signal. The report will explain capture is unavailable.
        if (oldAction.sa_handler != SIG_DFL && oldAction.sa_handler != SIG_IGN) return;
        struct sigaction action{};
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
        action.sa_sigaction = onCapture;
        if (sigaction(captureSignal, &action, nullptr) != 0) return;
        signalInstalled = true;
    }
    available = true;
#endif
}
void forceWalkerStallForTest(bool enabled) {
#if defined(_WIN32)
    je_hang_capture::stallWalker.store(enabled, std::memory_order_relaxed);
#else
    (void)enabled;
#endif
}
size_t captureMainThreadStack(char* out, size_t capacity, const char** why) {
    using namespace je_hang_capture;
    if (why) *why = "unavailable (capture signal reserved or backend disabled)";
    if (!out || capacity == 0 || !available) return 0;
#if defined(_WIN32)
    if (disabled.load(std::memory_order_acquire)) { if (why) *why = "capture timed out (disabled for session)"; return 0; }
    if (GetCurrentThreadId() == mainThreadId) {
        frameCount = Backtrace::capture(frames, maxFrames);
        formatCapturedFrames(false);
    } else {
        if (SuspendThread(mainThread) == static_cast<DWORD>(-1)) return 0;
        // From here the watchdog owns the resume, including every error and timeout path.
        capturedContext = {};
        capturedContext.ContextFlags = CONTEXT_FULL;
        const bool captured = GetThreadContext(mainThread, &capturedContext) != FALSE;
        DWORD waited = WAIT_FAILED;
        if (captured) {
            ResetEvent(readyEvent);
            SetEvent(requestEvent);
            waited = WaitForSingleObject(readyEvent, 250);
        }
        ResumeThread(mainThread);
        if (waited != WAIT_OBJECT_0) {
            disabled.store(true, std::memory_order_release);
            if (why) *why = "capture timed out (disabled for session)";
            return 0;
        }
    }
    return appendText(out, capacity, 0, capturedText);
#elif !defined(__EMSCRIPTEN__)
    if (disabled.load(std::memory_order_acquire)) { if (why) *why = "capture timed out (disabled for session)"; return 0; }
    if (pthread_equal(mainThread, pthread_self())) {
        frameCount = je_crash_posix::capture(frames, maxFrames);
    } else {
        requestState.store(1, std::memory_order_release);
        if (pthread_kill(mainThread, captureSignal) != 0) { requestState.store(0); return 0; }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        // Monotonic bounded polling works on Darwin too (unnamed semaphores do not). A
        // semaphore's realtime absolute timeout can overrun its budget after a clock step.
        while (requestState.load(std::memory_order_acquire) != 3 && std::chrono::steady_clock::now() < deadline) {
            const timespec delay{0, 1000000};
            nanosleep(&delay, nullptr);
        }
        if (requestState.load(std::memory_order_acquire) != 3) {
            disabled.store(true, std::memory_order_release);
            if (why) *why = "capture timed out (disabled for session)";
            return 0;
        }
        requestState.store(0, std::memory_order_release);
    }
#else
    return 0;
#endif
    size_t length = 0;
    for (size_t i = 0; i < frameCount; i++) {
        std::string description;
        Backtrace::describe(frames[i], description); // main thread has already resumed
        // The logger uses module(+offset) symbol; report readers use module+offset [symbol].
        const size_t offsetAt = description.find("(+0x");
        const size_t offsetEnd = offsetAt == std::string::npos ? std::string::npos : description.find(')', offsetAt);
        if (offsetEnd != std::string::npos) {
            std::string formatted = description.substr(0, offsetAt) + description.substr(offsetAt + 1, offsetEnd - offsetAt - 1);
            if (offsetEnd + 2 < description.size()) formatted += " [" + description.substr(offsetEnd + 2) + "]";
            description = std::move(formatted);
        }
        length = appendText(out, capacity, length, "#");
        length = appendDecimal(out, capacity, length, i);
        length = appendText(out, capacity, length, " ");
        length = appendText(out, capacity, length, description.c_str());
        length = appendText(out, capacity, length, "\n");
    }
    return length;
}
}
