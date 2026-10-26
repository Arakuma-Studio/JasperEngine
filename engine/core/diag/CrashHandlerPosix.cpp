// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Fatal path: open/write/close/mkdir, clock_gettime/nanosleep, sigaction/sigprocmask/raise,
// syscall(gettid), prctl, prewarmed backtrace/backtrace_symbols_fd (_Unwind_Backtrace on
// Android), __android_log_write, and Logger::flushForCrash (only atomics + clock/sleep).
// No stdio, heap allocation, symbol-loader calls or locks are permitted in that path.
#include "detail/CrashState.h"
#include "log/Logger.h"

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <cerrno>
#include <csignal>
#include <exception>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#if defined(__ANDROID__)
#include <android/log.h>
#include <unwind.h>
#elif defined(__GLIBC__) || defined(__APPLE__)
#include <execinfo.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#if defined(__aarch64__) || defined(__arm64__)
#include <mach/arm/thread_status.h>
#endif
#else
#include <link.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

namespace je_crash_posix {
using namespace jasperengine::diag::crash::detail;
constexpr int signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP};
struct sigaction previous[sizeof(signals) / sizeof(signals[0])]{};
struct Module { uintptr_t begin, end, base; char name[256]; };
Module modules[128]{};
size_t moduleCount = 0;
char moduleList[16384]{};
size_t moduleListBytes = 0;
// Native pthread identities are pointer/integer tokens on our POSIX platforms. Using a
// lock-free owner avoids initializing emulated C++ TLS from an Android signal handler.
static_assert(std::atomic<pthread_t>::is_always_lock_free);
std::atomic<pthread_t> reportOwner{};

struct AlternateStack {
    void* memory = MAP_FAILED;
    stack_t saved{};
    ~AlternateStack() {
        if (memory == MAP_FAILED) return;
        sigaltstack(&saved, nullptr);
        munmap(memory, 64 * 1024);
    }
};
thread_local AlternateStack alternate;
#if defined(__APPLE__)
thread_local char preparedThreadName[64]{};
thread_local int64_t preparedThreadId = 0;
#endif

void raw(int fd, const char* text, size_t size) {
    if (fd < 0) return;
    while (size) {
        const ssize_t n = write(fd, text, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        text += n;
        size -= static_cast<size_t>(n);
    }
}
void line(int fd, const char* text) {
    raw(fd, text, textLength(text));
    raw(fd, "\n", 1);
#if defined(__ANDROID__)
    __android_log_write(ANDROID_LOG_FATAL, "JasperEngine", text);
#endif
}
uint64_t monotonicNs() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return static_cast<uint64_t>(t.tv_sec) * 1000000000 + static_cast<uint64_t>(t.tv_nsec);
}
int64_t threadId() {
#if defined(__APPLE__)
    return preparedThreadId;
#else
    return static_cast<int64_t>(syscall(SYS_gettid));
#endif
}
const char* reason(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV (invalid memory access)";
        case SIGBUS: return "SIGBUS (bus error)";
        case SIGILL: return "SIGILL (illegal instruction)";
        case SIGFPE: return "SIGFPE (arithmetic error)";
        case SIGABRT: return "SIGABRT (abort)";
        case SIGTRAP: return "SIGTRAP (trap)";
        default: return "fatal signal";
    }
}
void registers(void* context, uintptr_t& pc, uintptr_t& sp) {
    if (!context) return;
    const auto* uc = static_cast<ucontext_t*>(context);
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    pc = arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
    sp = arm_thread_state64_get_sp(uc->uc_mcontext->__ss);
#elif defined(__APPLE__) && defined(__x86_64__)
    pc = uc->uc_mcontext->__ss.__rip;
    sp = uc->uc_mcontext->__ss.__rsp;
#elif defined(__aarch64__)
    pc = uc->uc_mcontext.pc;
    sp = uc->uc_mcontext.sp;
#elif defined(__x86_64__)
    pc = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
    sp = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RSP]);
#elif defined(__arm__)
    pc = uc->uc_mcontext.arm_pc;
    sp = uc->uc_mcontext.arm_sp;
#elif defined(__i386__)
    pc = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_EIP]);
    sp = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_ESP]);
#else
    (void)uc;
#endif
}
#if defined(__ANDROID__)
struct UnwindFrames { void** frames; size_t count, capacity; };
_Unwind_Reason_Code unwindFrame(_Unwind_Context* context, void* data) {
    auto& frames = *static_cast<UnwindFrames*>(data);
    if (frames.count == frames.capacity) return _URC_END_OF_STACK;
    const uintptr_t pc = _Unwind_GetIP(context);
    if (pc) frames.frames[frames.count++] = reinterpret_cast<void*>(pc);
    return _URC_NO_REASON;
}
#endif
size_t capture(void** frames, size_t capacity) {
#if defined(__ANDROID__)
    UnwindFrames out{frames, 0, capacity};
    _Unwind_Backtrace(unwindFrame, &out);
    return out.count;
#elif defined(__GLIBC__) || defined(__APPLE__)
    return static_cast<size_t>(backtrace(frames, static_cast<int>(capacity)));
#else
    (void)frames; (void)capacity;
    return 0;
#endif
}
void frame(int fd, size_t index, uintptr_t pc) {
    char text[768];
    size_t n = appendText(text, sizeof(text), 0, "#");
    n = appendDecimal(text, sizeof(text), n, index);
    n = appendText(text, sizeof(text), n, " ");
    uintptr_t base = 0;
    const char* name = "module?";
    for (size_t i = 0; i < moduleCount; i++) {
        if (pc >= modules[i].begin && pc < modules[i].end) {
            base = modules[i].base;
            name = modules[i].name;
            break;
        }
    }
    n = appendText(text, sizeof(text), n, name);
    n = appendText(text, sizeof(text), n, "+");
    appendHex(text, sizeof(text), n, pc - base);
    line(fd, text);
}
void number(int fd, const char* key, uint64_t value, bool hex = false) {
    char text[160];
    const size_t n = appendText(text, sizeof(text), 0, key);
    if (hex) appendHex(text, sizeof(text), n, value);
    else appendDecimal(text, sizeof(text), n, value);
    line(fd, text);
}
void ring(int fd) {
    const auto& s = state();
    if (!s.includeLogTail || !s.ring || !s.ringCapacity) return;
    const uint64_t cursor = s.ringCursor.load(std::memory_order_acquire);
    const size_t count = cursor < s.ringCapacity ? static_cast<size_t>(cursor) : s.ringCapacity;
    const size_t first = cursor <= s.ringCapacity ? 0 : static_cast<size_t>(cursor % s.ringCapacity);
    const size_t n = count < s.ringCapacity - first ? count : s.ringCapacity - first;
    raw(fd, s.ring + first, n);
    raw(fd, s.ring, count - n);
}
[[noreturn]] void defaultAction(int sig) {
    struct sigaction def{};
    def.sa_handler = SIG_DFL;
    sigemptyset(&def.sa_mask);
    sigaction(sig, &def, nullptr);
    sigset_t unblock;
    sigemptyset(&unblock);
    sigaddset(&unblock, sig);
    sigprocmask(SIG_UNBLOCK, &unblock, nullptr);
    raise(sig);
    _exit(128 + sig);
}
void chain(int sig, siginfo_t* info, void* context) {
    struct sigaction old{};
    old.sa_handler = SIG_DFL;
    sigemptyset(&old.sa_mask);
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
        if (signals[i] == sig) { old = previous[i]; break; }
    }
    // Restore first: sanitizers/debuggerd may re-raise and expect their own disposition.
    sigaction(sig, &old, nullptr);
    if (old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN) {
        if (old.sa_flags & SA_SIGINFO) old.sa_sigaction(sig, info, context);
        else old.sa_handler(sig);
        // A previous handler owns what happens next (it may fix the context, or debuggerd
        // may already have queued its own re-delivery). Do not replace that decision.
        return;
    }
    defaultAction(sig);
}
void fatal(int sig, siginfo_t* info, void* context) {
    auto& s = state();
    // Test the calling thread before touching shared state: a fault in the handler itself
    // cannot wait for its own report (or enter a possibly broken previous handler).
    const pthread_t current = pthread_self();
    pthread_t expected{};
    if (!reportOwner.compare_exchange_strong(expected, current, std::memory_order_acq_rel)) {
        if (expected == current && !s.reportDone.load(std::memory_order_acquire)) {
            defaultAction(sig);
        }
        const uint64_t deadline = monotonicNs() + 8000000000ULL;
        for (unsigned attempts = 0; attempts < 8000 && !s.reportDone.load(std::memory_order_acquire)
                && monotonicNs() < deadline; ++attempts) {
            const timespec pause{0, 1000000};
            nanosleep(&pause, nullptr);
        }
        // Do not add diagnostic I/O here: a full stderr pipe must not extend the wait.
        chain(sig, info, context);
        return;
    }
    s.reportingThread.store(threadId(), std::memory_order_relaxed);
    if (s.beforeReportForTesting) s.beforeReportForTesting();
    if (s.sessionDir[0]) mkdir(s.sessionDir, 0700);
    const int report = s.crashFile[0] ? open(s.crashFile, O_CREAT | O_WRONLY | O_TRUNC, 0600) : -1;
    raw(report, s.header, s.headerLength);
#if defined(__ANDROID__)
    char headerLine[1024];
    size_t used = 0;
    for (size_t i = 0; i < s.headerLength; i++) {
        if (s.header[i] == '\n') {
            headerLine[used] = 0;
            __android_log_write(ANDROID_LOG_FATAL, "JasperEngine", headerLine);
            used = 0;
        } else if (used + 1 < sizeof(headerLine)) headerLine[used++] = s.header[i];
    }
#endif
    const char* why = s.pendingReasonSet.load(std::memory_order_acquire) ? s.pendingReason : reason(sig);
    char text[1536];
    size_t n = appendText(text, sizeof(text), 0, "reason: ");
    appendText(text, sizeof(text), n, why);
    line(report, text);
    number(report, "signal: ", static_cast<uint64_t>(sig));
    char code[64];
    n = appendText(code, sizeof(code), 0, "si_code: ");
    appendSigned(code, sizeof(code), n, info ? info->si_code : 0);
    line(report, code);
    number(report, "fault_address: ", info ? reinterpret_cast<uintptr_t>(info->si_addr) : 0, true);
    uintptr_t pc = 0, sp = 0;
    registers(context, pc, sp);
    number(report, "pc: ", pc, true);
    number(report, "sp: ", sp, true);
    number(report, "thread: ", static_cast<uint64_t>(threadId()));
#if defined(__APPLE__)
    n = appendText(text, sizeof(text), 0, "thread_name: ");
    appendText(text, sizeof(text), n, preparedThreadName);
    line(report, text);
#else
    char threadName[16]{};
    prctl(PR_GET_NAME, threadName, 0, 0, 0);
    n = appendText(text, sizeof(text), 0, "thread_name: ");
    appendText(text, sizeof(text), n, threadName);
    line(report, text);
#endif
    number(report, "time_since_start_ms: ", (monotonicNs() - s.startMonotonicNs) / 1000000);
    if (s.deliberate.load(std::memory_order_relaxed)) line(report, "deliberate: true");
    for (const auto& tag : s.tags) {
        const uint32_t seq = tag.seq.load(std::memory_order_acquire);
        if (seq & 1) continue;
        n = appendText(text, sizeof(text), 0, tag.key);
        n = appendText(text, sizeof(text), n, ": ");
        appendText(text, sizeof(text), n, tag.value);
        if (tag.seq.load(std::memory_order_acquire) == seq && tag.key[0]) line(report, text);
    }
    void* frames[64];
    const size_t count = capture(frames, 64);
    // The unwinder starts inside this handler. Locate the saved interrupted PC, discard
    // everything through it (handler, libc trampoline and duplicate PC), then emit callers.
    // If the unwinder cannot cross the signal frame, only the saved PC is trustworthy.
    size_t callers = count;
    for (size_t i = 0; i < count; ++i) {
        uintptr_t address = reinterpret_cast<uintptr_t>(frames[i]);
#if defined(__arm__)
        address &= ~uintptr_t{1};
        const uintptr_t interrupted = pc & ~uintptr_t{1};
#else
        const uintptr_t interrupted = pc;
#endif
        if (pc && address == interrupted) { callers = i + 1; break; }
    }
    line(report, "backtrace:");
    if (pc) frame(report, 0, pc);
    for (size_t i = callers; i < count; i++) frame(report, i - callers + (pc ? 1 : 0), reinterpret_cast<uintptr_t>(frames[i]));
    line(report, "modules:");
#if defined(__APPLE__)
    raw(report, moduleList, moduleListBytes);
    line(report, "os_report: authoritative .ips in DiagnosticReports / Xcode Organizer / TestFlight");
#else
    const int maps = open("/proc/self/maps", O_RDONLY);
    if (maps >= 0) {
        char buffer[2048];
        ssize_t readBytes;
        while ((readBytes = read(maps, buffer, sizeof(buffer))) > 0) raw(report, buffer, static_cast<size_t>(readBytes));
        close(maps);
    }
#endif
    line(report, s.coreHint);
    if (report >= 0) close(report);
    const int tail = s.tailFile[0] ? open(s.tailFile, O_CREAT | O_WRONLY | O_TRUNC, 0600) : -1;
    ring(tail);
    if (tail >= 0) close(tail);

    // Preserve the editor.log contract: drain already queued records before its FATAL block.
    // This routine has no locks or allocation and detects the logger's own drain thread.
    uint64_t unwritten = 0;
    const bool flushed = jasperengine::Logger::flushForCrash(std::chrono::milliseconds(300), &unwritten);
    const int log = s.logFile[0] ? open(s.logFile, O_CREAT | O_WRONLY | O_APPEND, 0600) : -1;
    if (!flushed && unwritten) number(log, "log tail incomplete: ", unwritten);
    line(log, "\n========================================================");
    n = appendText(text, sizeof(text), 0, "FATAL: ");
    appendText(text, sizeof(text), n, why);
    line(log, text);
    line(log, s.banner);
    line(log, "backtrace:");
    if (pc) frame(log, 0, pc);
    for (size_t i = callers; i < count; i++) frame(log, i - callers + (pc ? 1 : 0), reinterpret_cast<uintptr_t>(frames[i]));
    if (log >= 0) close(log);
    n = appendText(text, sizeof(text), 0, "JASPERENGINE CRASH: ");
    n = appendText(text, sizeof(text), n, why);
    n = appendText(text, sizeof(text), n, " report=");
    appendText(text, sizeof(text), n, s.sessionDir);
    line(STDERR_FILENO, text);
    s.reportDone.store(true, std::memory_order_release);
    chain(sig, info, context);
}
void addModule(uintptr_t begin, uintptr_t end, uintptr_t base, const char* name) {
    // One entry per image, rather than copying its path for every load segment. Keep
    // basenames in frames: symbolication can use the build's symbols on another machine.
    for (size_t i = 0; i < moduleCount; i++) {
        if (modules[i].base != base) continue;
        if (begin < modules[i].begin) modules[i].begin = begin;
        if (end > modules[i].end) modules[i].end = end;
        return;
    }
    if (moduleCount == sizeof(modules) / sizeof(modules[0])) {
        // Android's app_process can load hundreds of system images before the game. Keep
        // our image even when the bounded cache is full; raw maps retain the evicted one.
        const uintptr_t owner = reinterpret_cast<uintptr_t>(&capture);
        if (owner < begin || owner >= end) return;
        --moduleCount;
    }
    auto& module = modules[moduleCount++];
    module.begin = begin; module.end = end; module.base = base;
    const char* basename = name;
    for (const char* c = name; *c; c++) if (*c == '/') basename = c + 1;
    appendText(module.name, sizeof(module.name), 0, basename);
#if defined(__APPLE__)
    moduleListBytes = appendHex(moduleList, sizeof(moduleList), moduleListBytes, base);
    moduleListBytes = appendText(moduleList, sizeof(moduleList), moduleListBytes, " ");
    moduleListBytes = appendText(moduleList, sizeof(moduleList), moduleListBytes, name);
    moduleListBytes = appendText(moduleList, sizeof(moduleList), moduleListBytes, "\n");
#endif
}
} // namespace je_crash_posix

namespace jasperengine::diag::crash {
void handleFatalSignal(int sig, siginfo_t* info, void* context) {
    if (!detail::state().installed.load(std::memory_order_acquire)) {
        je_crash_posix::chain(sig, info, context);
        return;
    }
    je_crash_posix::fatal(sig, info, context);
}
}
namespace jasperengine::diag::crash::detail {
void prepareThreadPlatform() {
    using namespace je_crash_posix;
#if defined(__APPLE__)
    // A fatal handler must not query pthread's name or take its internal locks.
    pthread_getname_np(pthread_self(), preparedThreadName, sizeof(preparedThreadName));
    uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    preparedThreadId = static_cast<int64_t>(id);
#endif
    if (alternate.memory != MAP_FAILED) return;
    stack_t current{};
    if (sigaltstack(nullptr, &current) != 0) return;
    // ASan may already provide a sufficiently large alternate stack; keep it.
    if (!(current.ss_flags & SS_DISABLE) && current.ss_size >= 64 * 1024) return;
    void* memory = mmap(nullptr, 64 * 1024, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (memory == MAP_FAILED) return;
    stack_t next{};
    next.ss_sp = memory; next.ss_size = 64 * 1024;
    if (sigaltstack(&next, nullptr) != 0) { munmap(memory, 64 * 1024); return; }
    alternate.saved = current;
    alternate.memory = memory;
}
bool installPlatform(const Config&) {
    using namespace je_crash_posix;
    state().startMonotonicNs = monotonicNs();
    prepareThreadPlatform();
    void* warm[16];
    capture(warm, 16);
    // Initialize the crash-flush globals and TLS before entering any signal context.
    jasperengine::Logger::flushForCrash(std::chrono::milliseconds(0));
#if defined(__APPLE__)
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const mach_header* image = _dyld_get_image_header(i);
        const intptr_t slide = _dyld_get_image_vmaddr_slide(i);
        const uint8_t* command = reinterpret_cast<const uint8_t*>(image) +
            (image->magic == MH_MAGIC_64 ? sizeof(mach_header_64) : sizeof(mach_header));
        for (uint32_t j = 0; j < image->ncmds; j++) {
            const auto* load = reinterpret_cast<const load_command*>(command);
            if (load->cmd == LC_SEGMENT_64) {
                const auto* segment = reinterpret_cast<const segment_command_64*>(load);
                if (segment->initprot) addModule(segment->vmaddr + slide, segment->vmaddr + slide + segment->vmsize,
                                               reinterpret_cast<uintptr_t>(image), _dyld_get_image_name(i));
            }
            command += load->cmdsize;
        }
    }
    appendText(state().coreHint, sizeof(state().coreHint), 0, "core: see the OS .ips report");
#else
    char executable[512]{};
    const ssize_t bytes = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (bytes > 0) executable[bytes] = 0;
    dl_iterate_phdr([](dl_phdr_info* info, size_t, void* data) -> int {
        const char* name = info->dlpi_name && *info->dlpi_name ? info->dlpi_name : static_cast<const char*>(data);
        for (size_t i = 0; i < info->dlpi_phnum; i++) {
            const auto& p = info->dlpi_phdr[i];
            if (p.p_type == PT_LOAD) addModule(info->dlpi_addr + p.p_vaddr, info->dlpi_addr + p.p_vaddr + p.p_memsz,
                                              info->dlpi_addr, name);
        }
        return 0;
    }, executable);
    char pattern[256]{};
    const int file = open("/proc/sys/kernel/core_pattern", O_RDONLY);
    if (file >= 0) { const ssize_t n = read(file, pattern, sizeof(pattern) - 1); if (n > 0) pattern[n] = 0; close(file); }
    size_t n = appendText(state().coreHint, sizeof(state().coreHint), 0, "core: ");
    appendText(state().coreHint, sizeof(state().coreHint), n, pattern[0] == '|' ? "use coredumpctl info / coredumpctl debug" : pattern);
#endif
    if (const char* enabled = std::getenv("JASPERENGINE_CRASH_CORE"); enabled && enabled[0] == '1') {
        rlimit limit{};
        if (getrlimit(RLIMIT_CORE, &limit) == 0) { limit.rlim_cur = limit.rlim_max; setrlimit(RLIMIT_CORE, &limit); }
    }
    struct sigaction action{};
    action.sa_sigaction = handleFatalSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    size_t installed = 0;
    for (int sig : signals) {
        if (sigaction(sig, &action, &previous[installed]) != 0) {
            for (size_t j = 0; j < installed; j++) sigaction(signals[j], &previous[j], nullptr);
            return false;
        }
        installed++;
    }
    return true;
}
bool platformDebugCrash(std::string_view) { return false; }
}
#endif
