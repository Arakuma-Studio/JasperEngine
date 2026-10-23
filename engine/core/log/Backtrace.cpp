// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Backtrace.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <windows.h>
    #include <dbghelp.h>
#elif defined(__EMSCRIPTEN__)
    // no stack walking
#elif defined(__ANDROID__)
    #include <dlfcn.h>
    #include <unwind.h>
    #include <cxxabi.h>
#else
    #include <dlfcn.h>
    #include <cxxabi.h>
    #if defined(__GLIBC__) || defined(__APPLE__)
        #include <execinfo.h>
        #define JASPERENGINE_BACKTRACE_EXECINFO 1
    #endif
#endif

using namespace jasperengine;

namespace {

#if defined(__ANDROID__)
    struct UnwindState {
        void** frames;
        size_t max;
        size_t skip;
        size_t count;
    };

    _Unwind_Reason_Code unwindFrame(struct _Unwind_Context* context, void* arg) {
        UnwindState* state = static_cast<UnwindState*>(arg);
        const uintptr_t pc = _Unwind_GetIP(context);
        if (pc == 0) return _URC_NO_REASON;
        if (state->skip > 0) {
            state->skip--;
            return _URC_NO_REASON;
        }
        if (state->count >= state->max) return _URC_END_OF_STACK;
        state->frames[state->count++] = reinterpret_cast<void*>(pc);
        return _URC_NO_REASON;
    }
#endif

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
    void appendFormatted(std::string& out, const char* format, const char* a, unsigned long long b) {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), format, a, b);
        out += buffer;
    }
#endif

}

std::timed_mutex& Backtrace::dbgHelpMutex() {
    // Never destroyed: a crash handler may still need it during exit
    static std::timed_mutex* mutex = new std::timed_mutex();
    return *mutex;
}

size_t Backtrace::capture(void** frames, size_t maxFrames, size_t skip) {
    if (!frames || maxFrames == 0) return 0;
    // this function's own frame
    skip += 1;
#if defined(_WIN32)
    return static_cast<size_t>(CaptureStackBackTrace(static_cast<DWORD>(skip), static_cast<DWORD>(maxFrames), frames, nullptr));
#elif defined(__EMSCRIPTEN__)
    (void)skip;
    return 0;
#elif defined(__ANDROID__)
    UnwindState state{frames, maxFrames, skip, 0};
    _Unwind_Backtrace(unwindFrame, &state);
    return state.count;
#elif defined(JASPERENGINE_BACKTRACE_EXECINFO)
    void* buffer[64 + 16];
    const size_t want = std::min<size_t>(maxFrames + skip, sizeof(buffer) / sizeof(buffer[0]));
    const int count = backtrace(buffer, static_cast<int>(want));
    size_t written = 0;
    for (int i = static_cast<int>(skip); i < count && written < maxFrames; i++) {
        frames[written++] = buffer[i];
    }
    return written;
#else
    (void)skip;
    return 0;
#endif
}

void Backtrace::describe(const void* address, std::string& out) {
#if defined(_WIN32)
    char moduleName[MAX_PATH] = {0};
    unsigned long long moduleOffset = 0;
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(address), &module) && module) {
        GetModuleFileNameA(module, moduleName, static_cast<DWORD>(sizeof(moduleName)));
        moduleOffset = static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module));
    }
    char buffer[MAX_PATH + 320];
    if (!module) {
        std::snprintf(buffer, sizeof(buffer), "module?+0x%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address)));
        out += buffer;
        return;
    }
    const char* moduleBase = moduleName;
    for (const char* c = moduleName; *c; c++) {
        if (*c == '\\' || *c == '/') moduleBase = c + 1;
    }

    bool named = false;
    std::timed_mutex& mutex = dbgHelpMutex();
    if (mutex.try_lock_for(std::chrono::milliseconds(200))) {
        static bool symbolsReady = false;
        static bool symbolsTried = false;
        HANDLE process = GetCurrentProcess();
        if (!symbolsTried) {
            symbolsTried = true;
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
            symbolsReady = SymInitialize(process, nullptr, TRUE) != FALSE;
        }
        if (symbolsReady) {
            // modules loaded since (a C++ script library) are not in the initial list
            SymRefreshModuleList(process);
            char symbolBuffer[sizeof(SYMBOL_INFO) + 256] = {0};
            SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            DWORD64 displacement = 0;
            if (SymFromAddr(process, static_cast<DWORD64>(reinterpret_cast<uintptr_t>(address)), &displacement, symbol)) {
                std::snprintf(buffer, sizeof(buffer), "%s(+0x%llx) %s+0x%llx", moduleBase, moduleOffset, symbol->Name,
                              static_cast<unsigned long long>(displacement));
                named = true;
            }
        }
        mutex.unlock();
    }
    if (!named) {
        std::snprintf(buffer, sizeof(buffer), "%s(+0x%llx)", moduleBase, moduleOffset);
    }
    out += buffer;
#elif defined(__EMSCRIPTEN__)
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "module?+0x%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address)));
    out += buffer;
#else
    Dl_info info{};
    if (!dladdr(address, &info) || !info.dli_fname) {
        char buffer[48];
        std::snprintf(buffer, sizeof(buffer), "module?+0x%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address)));
        out += buffer;
        return;
    }
    const char* moduleBase = info.dli_fname;
    for (const char* c = info.dli_fname; *c; c++) {
        if (*c == '/') moduleBase = c + 1;
    }
    // a return address points past the call: one byte back is inside the calling instruction
    const uintptr_t pc = reinterpret_cast<uintptr_t>(address);
    const unsigned long long moduleOffset = static_cast<unsigned long long>(pc - reinterpret_cast<uintptr_t>(info.dli_fbase));
    appendFormatted(out, "%s(+0x%llx)", moduleBase, moduleOffset);
    if (info.dli_sname) {
        int status = 0;
        char* demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
        out += ' ';
        out += (status == 0 && demangled) ? demangled : info.dli_sname;
        std::free(demangled);
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "+0x%llx",
                      static_cast<unsigned long long>(pc - reinterpret_cast<uintptr_t>(info.dli_saddr)));
        out += buffer;
    }
#endif
}
