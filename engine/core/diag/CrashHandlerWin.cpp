// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <crtdbg.h>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <mutex>

#include "detail/CrashState.h"
#include "log/Backtrace.h"
#include "log/Logger.h"

namespace je_crash_win {
using namespace jasperengine;
using namespace diag::crash;
using namespace diag::crash::detail;
HANDLE request = nullptr, completed = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER previousFilter = nullptr;
using AbortHandler = void (*)(int);
AbortHandler previousAbort = SIG_DFL;
EXCEPTION_RECORD faultRecord{};
CONTEXT faultContext{};
EXCEPTION_POINTERS faultPointers{&faultRecord, &faultContext};
DWORD faultThread = 0;
HANDLE faultThreadHandle = nullptr;
std::atomic<DWORD> faultOwner{0};
DWORD writerThread = 0;
using GetThreadDescriptionFn = HRESULT (WINAPI*)(HANDLE, PWSTR*);
// Plain native TLS is initialized when the thread starts, with no lazy constructor in
// the fatal path. prepareThreadPlatform refreshes it after spawnNamed sets the OS name.
thread_local char preparedThreadNames[2][256] = {"unknown", "unknown"};
thread_local std::atomic<unsigned> preparedNameIndex{0};
static_assert(std::atomic<unsigned>::is_always_lock_free);
char faultThreadName[256] = "unknown";

void raw(HANDLE file, const char* text, size_t size) {
    if (!file || file == INVALID_HANDLE_VALUE) return;
    while (size) {
        DWORD written = 0;
        const DWORD bytes = static_cast<DWORD>(size > MAXDWORD ? MAXDWORD : size);
        if (!WriteFile(file, text, bytes, &written, nullptr) || !written) break;
        text += written; size -= written;
    }
}
void line(HANDLE file, const char* text) { raw(file, text, textLength(text)); raw(file, "\n", 1); }
// The report paths are UTF-8 (a user folder named "José" or "张伟"): widened on the stack, so
// nothing allocates and nothing depends on the ANSI code page (games run before Windows 10 1903)
bool widePath(const char* path, wchar_t (&wide)[kPathBytes]) {
    return *path && MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, static_cast<int>(kPathBytes)) > 0;
}
HANDLE openFile(const char* path, bool append = false) {
    wchar_t wide[kPathBytes];
    if (!widePath(path, wide)) return INVALID_HANDLE_VALUE;
    return CreateFileW(wide, append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       nullptr, append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}
void number(HANDLE file, const char* label, uint64_t value, bool hex = false) {
    char buffer[256];
    const size_t n = appendText(buffer, sizeof(buffer), 0, label);
    if (hex) appendHex(buffer, sizeof(buffer), n, value);
    else appendDecimal(buffer, sizeof(buffer), n, value);
    line(file, buffer);
}
void ring(HANDLE file) {
    const auto& s = state();
    if (!s.includeLogTail || !s.ring || !s.ringCapacity) return;
    const uint64_t cursor = s.ringCursor.load(std::memory_order_acquire);
    const size_t count = cursor < s.ringCapacity ? static_cast<size_t>(cursor) : s.ringCapacity;
    const size_t first = cursor <= s.ringCapacity ? 0 : static_cast<size_t>(cursor % s.ringCapacity);
    const size_t n = count < s.ringCapacity - first ? count : s.ringCapacity - first;
    raw(file, s.ring + first, n); raw(file, s.ring, count - n);
}
DWORD frameContext(CONTEXT& context, STACKFRAME64& frame) {
    frame.AddrPC.Mode = frame.AddrStack.Mode = frame.AddrFrame.Mode = AddrModeFlat;
#if defined(_M_X64) || defined(__x86_64__)
    frame.AddrPC.Offset = context.Rip; frame.AddrStack.Offset = context.Rsp; frame.AddrFrame.Offset = context.Rbp;
    return IMAGE_FILE_MACHINE_AMD64;
#elif defined(_M_ARM64) || defined(__aarch64__)
    frame.AddrPC.Offset = context.Pc; frame.AddrStack.Offset = context.Sp; frame.AddrFrame.Offset = context.Fp;
    return IMAGE_FILE_MACHINE_ARM64;
#else
    frame.AddrPC.Offset = context.Eip; frame.AddrStack.Offset = context.Esp; frame.AddrFrame.Offset = context.Ebp;
    return IMAGE_FILE_MACHINE_I386;
#endif
}
void writeFrame(HANDLE file, size_t index, DWORD64 pc, bool symbols) {
    char buffer[1536];
    size_t n = appendText(buffer, sizeof(buffer), 0, "#");
    n = appendDecimal(buffer, sizeof(buffer), n, index);
    n = appendText(buffer, sizeof(buffer), n, " ");
    // VirtualQuery needs no loader lock, so module+offset is available even if DbgHelp's
    // lock was held by the faulting thread. The base's address still identifies the module.
    MEMORY_BASIC_INFORMATION memory{};
    const uintptr_t base = VirtualQuery(reinterpret_cast<const void*>(pc), &memory, sizeof(memory))
        ? reinterpret_cast<uintptr_t>(memory.AllocationBase) : 0;
    char name[MAX_PATH]{};
    IMAGEHLP_MODULE64 module{};
    module.SizeOfStruct = sizeof(module);
    if (symbols && SymGetModuleInfo64(static_cast<HANDLE>(Backtrace::dbgHelpProcess()), pc, &module)) {
        appendText(name, sizeof(name), 0, module.ImageName);
    }
    const char* basename = name;
    for (const char* c = name; *c; c++) if (*c == '/' || *c == '\\') basename = c + 1;
    if (name[0]) n = appendText(buffer, sizeof(buffer), n, basename);
    else { n = appendText(buffer, sizeof(buffer), n, "module@"); n = appendHex(buffer, sizeof(buffer), n, base); }
    n = appendText(buffer, sizeof(buffer), n, "+");
    n = appendHex(buffer, sizeof(buffer), n, pc - base);
    if (symbols) {
        alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256]{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO); symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        if (SymFromAddr(static_cast<HANDLE>(Backtrace::dbgHelpProcess()), pc, &displacement, symbol)) {
            n = appendText(buffer, sizeof(buffer), n, " [");
            n = appendText(buffer, sizeof(buffer), n, symbol->Name);
            n = appendText(buffer, sizeof(buffer), n, "+");
            n = appendHex(buffer, sizeof(buffer), n, displacement);
            appendText(buffer, sizeof(buffer), n, "]");
        }
    }
    line(file, buffer);
}
MINIDUMP_TYPE dumpType() {
    ULONG flags = MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules;
    if (state().dumpKind != DumpKind::Small) flags |= MiniDumpWithIndirectlyReferencedMemory;
    if (state().dumpKind == DumpKind::Editor) {
        flags |= MiniDumpWithHandleData;
#ifndef NDEBUG
        flags |= MiniDumpWithFullMemory;
#endif
    }
    return static_cast<MINIDUMP_TYPE>(flags);
}
void cacheThreadName() {
    // GetThreadDescription allocates. This runs only in normal thread preparation,
    // never on the faulting path or writer, where the process heap may be locked.
    auto getThreadDescription = reinterpret_cast<GetThreadDescriptionFn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
    char name[256] = "unknown";
    PWSTR wide = nullptr;
    if (getThreadDescription && SUCCEEDED(getThreadDescription(GetCurrentThread(), &wide)) && wide) {
        if (*wide && !WideCharToMultiByte(CP_UTF8, 0, wide, -1, name,
                                        sizeof(name), nullptr, nullptr)) {
            appendText(name, sizeof(name), 0, "unknown");
        }
        LocalFree(wide);
    }
    // Publish a complete UTF-8 snapshot. A fault during conversion/free or the copy
    // still reads the previous buffer, without a partial string or first-use allocation.
    const unsigned next = (preparedNameIndex.load(std::memory_order_relaxed) ^ 1u) & 1u;
    appendText(preparedThreadNames[next], sizeof(preparedThreadNames[next]), 0, name);
    preparedNameIndex.store(next, std::memory_order_release);
}
void writeThreadName(HANDLE report) {
    char lineText[288];
    const size_t n = appendText(lineText, sizeof(lineText), 0, "thread_name: ");
    appendText(lineText, sizeof(lineText), n, faultThreadName);
    line(report, lineText);
}
DWORD WINAPI writer(void*) {
    if (WaitForSingleObject(request, INFINITE) != WAIT_OBJECT_0) return 0;
    auto& s = state();
    if (s.beforeReportForTesting) s.beforeReportForTesting();
    wchar_t sessionDir[kPathBytes];
    if (widePath(s.sessionDir, sessionDir)) CreateDirectoryW(sessionDir, nullptr);
    // DbgHelp is process-wide and single threaded. Never wait indefinitely on a lock the
    // faulting thread may own. A failed lock still yields a report with its saved PC/SP.
    std::unique_lock<std::timed_mutex> lock(Backtrace::dbgHelpMutex(), std::defer_lock);
    const bool locked = lock.try_lock_for(std::chrono::milliseconds(200));
    const bool symbols = locked && Backtrace::initializeSymbolsLocked();
    bool dumped = false;
    if (locked) {
        HANDLE dump = openFile(s.dumpFile);
        MINIDUMP_EXCEPTION_INFORMATION exception{faultThread, &faultPointers, FALSE};
        if (dump != INVALID_HANDLE_VALUE) {
            dumped = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump, dumpType(), &exception, nullptr, nullptr) != FALSE;
            CloseHandle(dump);
        }
    }
    const char* why = s.pendingReasonSet.load(std::memory_order_acquire) ? s.pendingReason : "unhandled Windows exception";
    HANDLE report = openFile(s.crashFile);
    raw(report, s.header, s.headerLength);
    char buffer[1536];
    size_t n = appendText(buffer, sizeof(buffer), 0, "reason: ");
    appendText(buffer, sizeof(buffer), n, why);
    line(report, buffer);
    number(report, "exception_code: ", faultRecord.ExceptionCode, true);
    number(report, "fault_address: ", faultRecord.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && faultRecord.NumberParameters >= 2
        ? faultRecord.ExceptionInformation[1] : reinterpret_cast<uintptr_t>(faultRecord.ExceptionAddress), true);
    number(report, "thread: ", faultThread);
    writeThreadName(report);
    CONTEXT context = faultContext;
    STACKFRAME64 frame{};
    const DWORD machine = frameContext(context, frame);
    number(report, "pc: ", frame.AddrPC.Offset, true);
    number(report, "sp: ", frame.AddrStack.Offset, true);
    const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    number(report, "time_since_start_ms: ", (now - s.startMonotonicNs) / 1000000);
    line(report, dumped ? "minidump: crash.dmp" : "minidump: unavailable (DbgHelp busy or write failed)");
    for (const auto& tag : s.tags) {
        const uint32_t seq = tag.seq.load(std::memory_order_acquire);
        if (seq & 1) continue;
        n = appendText(buffer, sizeof(buffer), 0, tag.key);
        n = appendText(buffer, sizeof(buffer), n, ": ");
        appendText(buffer, sizeof(buffer), n, tag.value);
        if (tag.seq.load(std::memory_order_acquire) == seq && tag.key[0]) line(report, buffer);
    }
    line(report, "backtrace:");
    DWORD64 addresses[64]{};
    size_t count = 0;
    if (frame.AddrPC.Offset) addresses[count++] = frame.AddrPC.Offset;
    if (symbols) {
        while (count < 64 && StackWalk64(machine, static_cast<HANDLE>(Backtrace::dbgHelpProcess()), faultThreadHandle, &frame, &context, nullptr,
                                        SymFunctionTableAccess64, SymGetModuleBase64, nullptr) && frame.AddrPC.Offset) {
            addresses[count++] = frame.AddrPC.Offset;
        }
    }
    for (size_t i = 0; i < count; i++) writeFrame(report, i, addresses[i], symbols);
    line(report, "modules:");
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W module{};
        module.dwSize = sizeof(module);
        if (Module32FirstW(snapshot, &module)) do {
            char exePath[MAX_PATH * 3] = "";
            WideCharToMultiByte(CP_UTF8, 0, module.szExePath, -1, exePath, static_cast<int>(sizeof(exePath)), nullptr, nullptr);
            n = appendText(buffer, sizeof(buffer), 0, exePath);
            n = appendText(buffer, sizeof(buffer), n, " ");
            n = appendHex(buffer, sizeof(buffer), n, reinterpret_cast<uintptr_t>(module.modBaseAddr));
            n = appendText(buffer, sizeof(buffer), n, " size=");
            appendHex(buffer, sizeof(buffer), n, module.modBaseSize);
            line(report, buffer);
        } while (Module32NextW(snapshot, &module));
        CloseHandle(snapshot);
    }
    if (report != INVALID_HANDLE_VALUE) CloseHandle(report);
    HANDLE tail = openFile(s.tailFile); ring(tail); if (tail != INVALID_HANDLE_VALUE) CloseHandle(tail);
    uint64_t unwritten = 0;
    const bool flushed = Logger::flushForCrash(std::chrono::milliseconds(300), &unwritten);
    HANDLE log = openFile(s.logFile, true);
    if (!flushed && unwritten) number(log, "log tail incomplete: ", unwritten);
    line(log, "\n========================================================");
    n = appendText(buffer, sizeof(buffer), 0, "FATAL: ");
    appendText(buffer, sizeof(buffer), n, why);
    line(log, buffer); line(log, s.banner); line(log, "backtrace:");
    for (size_t i = 0; i < count; i++) writeFrame(log, i, addresses[i], symbols);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    n = appendText(buffer, sizeof(buffer), 0, "JASPERENGINE CRASH: ");
    n = appendText(buffer, sizeof(buffer), n, why);
    n = appendText(buffer, sizeof(buffer), n, " report=");
    appendText(buffer, sizeof(buffer), n, s.sessionDir);
    line(GetStdHandle(STD_ERROR_HANDLE), buffer);
    s.reportDone.store(true, std::memory_order_release);
    SetEvent(completed);
    return 0;
}
void submit(EXCEPTION_POINTERS* info) {
    const DWORD current = GetCurrentThreadId();
    // A recursive exception on either the submitting thread or the writer must reach the
    // OS immediately. Other faulting threads wait for the complete report before chaining.
    if (current == writerThread) {
        return;
    }
    DWORD expected = 0;
    if (!faultOwner.compare_exchange_strong(expected, current, std::memory_order_acq_rel)) {
        if (expected == current) {
            return;
        }
        WaitForSingleObject(completed, 8000);
        return;
    }
    faultThread = current;
    const unsigned nameIndex = preparedNameIndex.load(std::memory_order_acquire) & 1u;
    appendText(faultThreadName, sizeof(faultThreadName), 0, preparedThreadNames[nameIndex]);
    faultRecord = *info->ExceptionRecord;
    faultRecord.ExceptionRecord = nullptr;
    faultContext = *info->ContextRecord;
    faultThreadHandle = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, faultThread);
    SetEvent(request);
    // The writer may encounter a loader/heap lock held by this thread; never wedge exit.
    WaitForSingleObject(completed, 8000);
}
LONG WINAPI filter(EXCEPTION_POINTERS* info) {
    const DWORD current = GetCurrentThreadId();
    if (current == writerThread || (faultOwner.load(std::memory_order_acquire) == current &&
            !state().reportDone.load(std::memory_order_acquire))) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (info && info->ExceptionRecord && info->ContextRecord) submit(info);
    if (previousFilter && previousFilter != filter) return previousFilter(info);
    return EXCEPTION_CONTINUE_SEARCH; // WER/debuggers still receive the original exception
}
void synthetic(const char* reason, DWORD code) {
    auto& s = state();
    const DWORD current = GetCurrentThreadId();
    if (current == writerThread || (faultOwner.load(std::memory_order_acquire) == current &&
            !s.reportDone.load(std::memory_order_acquire))) {
        // The CRT default is process termination. Do not enter an arbitrary saved abort
        // callback (or wait for the writer) when the reporter itself has faulted again.
        TerminateProcess(GetCurrentProcess(), code);
        std::_Exit(3);
    }
    if (!s.pendingReasonSet.load(std::memory_order_relaxed)) {
        appendText(s.pendingReason, sizeof(s.pendingReason), 0, reason);
        s.pendingReasonSet.store(true, std::memory_order_release);
    }
    CONTEXT context{};
    RtlCaptureContext(&context);
    STACKFRAME64 frame{};
    frameContext(context, frame);
    EXCEPTION_RECORD exception{};
    exception.ExceptionCode = code;
    exception.ExceptionAddress = reinterpret_cast<void*>(frame.AddrPC.Offset);
    EXCEPTION_POINTERS pointers{&exception, &context};
    submit(&pointers);
}
void onAbort(int sig) {
    synthetic("SIGABRT (abort)", 0x40000015);
    if (previousAbort != SIG_DFL && previousAbort != SIG_IGN && previousAbort != onAbort) previousAbort(sig);
    std::signal(SIGABRT, SIG_DFL);
    std::raise(SIGABRT);
    std::_Exit(3);
}
void onPureCall() { synthetic("pure virtual function call", 0xc0000025); std::abort(); }
void onInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
    synthetic("invalid CRT parameter", 0xc000000d); std::abort();
}
}
namespace jasperengine::diag::crash::detail {
void prepareThreadPlatform() {
    ULONG bytes = 64 * 1024;
    SetThreadStackGuarantee(&bytes);
    je_crash_win::cacheThreadName();
}
bool installPlatform(const Config&) {
    using namespace je_crash_win;
    prepareThreadPlatform();
    request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!request || !completed) return false;
    {
        std::lock_guard<std::timed_mutex> lock(Backtrace::dbgHelpMutex());
        Backtrace::initializeSymbolsLocked();
    }
    HANDLE worker = CreateThread(nullptr, 0, writer, nullptr, 0, &writerThread);
    if (!worker) return false;
    CloseHandle(worker);
    previousFilter = SetUnhandledExceptionFilter(filter);
    previousAbort = std::signal(SIGABRT, onAbort);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_purecall_handler(onPureCall);
    _set_invalid_parameter_handler(onInvalidParameter);
    return true;
}
bool platformDebugCrash(std::string_view mode) {
    if (mode == "pure_call") { je_crash_win::onPureCall(); return true; }
    return false;
}
}
#endif
