// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Logger.h"
#include "profiler/Profile.h"
#include "Backtrace.h"
// timed waits TSan can see (GCC 11 misses libstdc++'s steady-clock wait)
#include "jobs/detail/TimedWait.h"
#include "util/StringUtils.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <windows.h>
    #include <intrin.h>
#else
    #include <pthread.h>
    #include <time.h>
    #include <unistd.h>
    #if (defined(__linux__) || defined(__ANDROID__)) && !defined(__EMSCRIPTEN__)
        #include <sys/prctl.h>
        #include <sys/syscall.h>
    #endif
#endif

using namespace jasperengine;

namespace je_logger {}
using namespace je_logger;

namespace je_logger {

    constexpr size_t kMaxSinks = 16;
    constexpr size_t kMaxCategories = LogCategory::MAX_CATEGORIES;
    constexpr uint8_t kInheritLevel = 0xFF;
    // the drain thread's longest sleep: how stale a file line may get (Warn+ wakes it at once)
    constexpr std::chrono::milliseconds kDrainInterval{20};
    // Warn and above wait this long for queue space before they are dropped
    constexpr std::chrono::milliseconds kQueueFullWait{5};
    // the rate limit: records per key per second, and how many keys are tracked
    constexpr uint32_t kValveLimit = 200;
    constexpr size_t kValveSlots = 256;
    // the table in stripes, each with a lock of its own: producers of different call sites
    // rarely meet
    constexpr size_t kValveStripes = 16;
    constexpr size_t kValveStripeSlots = kValveSlots / kValveStripes;
    constexpr size_t kValveProbe = 8;
    constexpr size_t kValveSampleBytes = 80;
    constexpr int64_t kValveWindowMs = 1000;
    constexpr uint32_t kBacktracesPerSecond = 10;

    // ------------------------------------------------------------ categories

    struct CategorySlot {
        LogCategory category{};
        char name[32] = {0};
        std::atomic<uint8_t> level{static_cast<uint8_t>(LogLevel::Info)};
    };

    // ------------------------------------------------------------ sinks

    struct SinkEntry {
        Logger::SinkId id = 0;
        std::unique_ptr<LogSink> sink;
        bool inlineDelivery = false;
        bool resident = false;       // LogSink::resident (Inline only)
        bool afterWrite = false;     // LogSink::wantsAfterWrite (Inline only)
        // held while the sink writes: the drain thread's batch, a Fatal record
        std::mutex writeMutex;
        std::array<std::atomic<uint8_t>, kMaxCategories> categoryLevels;

        SinkEntry() {
            for (auto& level : categoryLevels) level.store(kInheritLevel, std::memory_order_relaxed);
        }

        uint8_t levelFor(const LogCategory& category) const {
            if (!(sink->categoryMask.load(std::memory_order_relaxed) & (1ull << category.index))) {
                return static_cast<uint8_t>(LogLevel::Off);
            }
            const uint8_t own = categoryLevels[category.index].load(std::memory_order_relaxed);
            return own != kInheritLevel ? own : static_cast<uint8_t>(sink->minLevel.load(std::memory_order_relaxed));
        }

        bool accepts(LogLevel level, const LogCategory& category) const {
            return static_cast<uint8_t>(level) >= levelFor(category);
        }
    };

    using SinkPtr = std::shared_ptr<SinkEntry>;

    // ------------------------------------------------------------ queue entries

    enum : uint8_t { kEntryRecord = 0, kEntryPadding = 1 };

    // What the byte ring holds per record, followed by the frames, file, function, thread
    // name and text; every entry is 8-byte aligned
    struct EntryHeader {
        uint32_t size;
        uint8_t kind;
        uint8_t level;
        uint8_t category;
        uint8_t frameCount;
        uint32_t flags;
        uint32_t line;
        uint32_t textLength;
        uint16_t fileLength;
        uint16_t functionLength;
        uint8_t threadNameLength;
        uint8_t reserved[3];
        uint32_t excludedSink;   // logged from inside this sink: it does not get the record
        uint64_t id;
        double wallTime;
        uint64_t frame;
        uint64_t threadId;
    };
    static_assert(sizeof(EntryHeader) % 8 == 0, "queue entries stay 8-byte aligned");

    size_t logAlignUp(size_t value) {
        return (value + 7) & ~static_cast<size_t>(7);
    }

    // ------------------------------------------------------------ the rate limit

    struct ValveSlot {
        uint64_t key = 0;
        int64_t windowStartMs = 0;
        uint32_t count = 0;
        uint32_t suppressed = 0;
        uint8_t level = 0;
        uint8_t category = 0;
        char sample[kValveSampleBytes + 1] = {0};
    };

    struct ValveSummary {
        uint32_t suppressed;
        uint8_t level;
        uint8_t category;
        char sample[kValveSampleBytes + 1];
    };

    // ------------------------------------------------------------ per thread

    // Constant-initialized TLS: crash flush must not construct ThreadState in a signal.
    thread_local bool crashDrainThread = false;

    struct ThreadState {
        uint64_t id = 0;
        char name[LogRecord::MAX_THREAD_NAME_BYTES] = {0};
        bool named = false;
        bool isDrainThread = false;
        bool holdsQueueLock = false;
        // the sink being written on this thread (a record logged from inside it skips it)
        uint32_t currentSink = 0;
        char formatBuffer[1024];
        std::string largeText;
        void* frames[LogRecord::MAX_FRAMES];
        // Inline sinks written under the lock that want afterWrite() once it is released
        std::vector<std::shared_ptr<SinkEntry>> afterWrite;
    };

    ThreadState& logThreadState() {
        thread_local ThreadState state;
        return state;
    }

    uint64_t logOsThreadId() {
#if defined(_WIN32)
        return static_cast<uint64_t>(GetCurrentThreadId());
#elif defined(__EMSCRIPTEN__)
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pthread_self()));
#elif defined(__APPLE__)
        uint64_t id = 0;
        pthread_threadid_np(nullptr, &id);
        return id;
#elif defined(__linux__) || defined(__ANDROID__)
        return static_cast<uint64_t>(syscall(SYS_gettid));
#else
        return 0;
#endif
    }

    void logOsThreadName(char* out, size_t size) {
        out[0] = '\0';
#if defined(__EMSCRIPTEN__)
        (void)size;
#elif defined(__linux__) || defined(__ANDROID__)
        char name[17] = {0};
        if (prctl(PR_GET_NAME, name, 0, 0, 0) == 0) {
            std::snprintf(out, size, "%s", name);
        }
#elif defined(__APPLE__)
        pthread_getname_np(pthread_self(), out, size);
#else
        (void)size;
#endif
    }

    void logSetOsThreadName(const char* name) {
#if defined(__EMSCRIPTEN__)
        (void)name;
#elif defined(__APPLE__)
        pthread_setname_np(name);
#elif defined(__linux__) || defined(__ANDROID__)
        pthread_setname_np(pthread_self(), name);
#elif defined(_WIN32)
        // Windows 10 1607 and later; looked up so older systems still run
        using SetThreadDescriptionFn = HRESULT (WINAPI*)(HANDLE, PCWSTR);
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        auto setDescription = kernel ? reinterpret_cast<SetThreadDescriptionFn>(
            reinterpret_cast<void*>(GetProcAddress(kernel, "SetThreadDescription"))) : nullptr;
        if (setDescription) {
            wchar_t wide[64] = {0};
            for (size_t i = 0; i + 1 < 64 && name[i]; i++) wide[i] = static_cast<wchar_t>(name[i]);
            setDescription(GetCurrentThread(), wide);
        }
#else
        (void)name;
#endif
    }

    ThreadState& logCurrentThread() {
        ThreadState& state = logThreadState();
        if (state.id == 0) {
            state.id = logOsThreadId();
            if (state.id == 0) state.id = 1;
        }
        if (!state.named) {
            state.named = true;
            logOsThreadName(state.name, sizeof(state.name));
        }
        return state;
    }

    void logCpuRelax() {
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
        __builtin_ia32_pause();
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
        _mm_pause();
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
        asm volatile("yield");
#endif
    }

    // Monotonic milliseconds; async-signal-safe (clock_gettime / GetTickCount64)
    int64_t logMonotonicMs() {
#if defined(_WIN32)
        return static_cast<int64_t>(GetTickCount64());
#else
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
#endif
    }

    void logSleepMs(int ms) {
#if defined(_WIN32)
        Sleep(static_cast<DWORD>(ms));
#else
        timespec duration{ms / 1000, (ms % 1000) * 1000000L};
        nanosleep(&duration, nullptr);
#endif
    }

    double logWallSeconds() {
        return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    uint64_t logHashText(std::string_view text) {
        // FNV-1a over at most the first 64 bytes
        uint64_t hash = 1469598103934665603ull;
        const size_t length = std::min<size_t>(text.size(), 64);
        for (size_t i = 0; i < length; i++) {
            hash ^= static_cast<unsigned char>(text[i]);
            hash *= 1099511628211ull;
        }
        return hash;
    }

    bool isPassThroughFormat(const char* format) {
        return format && format[0] == '%' && format[1] == 's' && (format[2] == '\0' || (format[2] == '\n' && format[3] == '\0'));
    }

    // ------------------------------------------------------------ the logger

    struct LoggerState {
        // categories: registered under categoryMutex, read lock-free (count only grows)
        std::mutex categoryMutex;
        std::array<CategorySlot, kMaxCategories> categories;
        std::atomic<size_t> categoryCount{0};
        std::atomic<uint8_t> defaultLevel{static_cast<uint8_t>(LogLevel::Info)};

        // the queue, the sinks and ids
        std::mutex queueMutex;
        std::condition_variable drainCv;      // records waiting / stop
        std::condition_variable spaceCv;      // queue space freed
        std::condition_variable drainedCv;    // a batch done (flush)
        std::array<SinkPtr, kMaxSinks> sinks;
        size_t sinkCount = 0;
        std::atomic<uint32_t> residentCount{0};   // resident sinks among them
        Logger::SinkId nextSinkId = 1;
        uint64_t sinkGeneration = 0;
        std::unique_ptr<uint8_t[]> ring;
        size_t capacity = 0;
        size_t head = 0;
        size_t tail = 0;
        size_t used = 0;
        size_t highWater = 0;
        bool drainSleeping = false;
        bool stopping = false;
        std::thread* drainThread = nullptr;   // never destroyed while running (exit paths)
        std::atomic<bool> initialized{false};
        std::atomic<bool> async{false};
        std::atomic<uint64_t> lastId{0};
        std::atomic<uint64_t> pushedSeq{0};    // records put in the queue
        std::atomic<uint64_t> drainedSeq{0};   // of those, written by the drain thread
        std::atomic<uint64_t> dropped{0};
        uint64_t droppedReported = 0;

        std::atomic<uint64_t> frame{0};
        std::atomic<bool> backtraceOnError{false};

        std::mutex backtraceMutex;
        int64_t backtraceWindowMs = 0;
        uint32_t backtraceCount = 0;
        std::atomic<uint64_t> backtracesOmitted{0};

        std::array<std::mutex, kValveStripes> valveMutexes;
        std::array<ValveSlot, kValveSlots> valve;
        std::atomic<int64_t> nextValveSweepMs{0};
        std::atomic<uint64_t> suppressed{0};
    };

    LoggerState& logState() {
        // Never destroyed: logging must keep working from static destructors and exit paths
        static LoggerState* instance = new LoggerState();
        return *instance;
    }

    void writeFallback(LogLevel level, const LogCategory* category, std::string_view text) {
        const char* prefix = "";
        switch (level) {
            case LogLevel::Trace: prefix = "(VERBOSE): "; break;
            case LogLevel::Debug: prefix = "(DEBUG): "; break;
            case LogLevel::Warn:  prefix = "(WARN): "; break;
            case LogLevel::Error: prefix = "(ERROR): "; break;
            case LogLevel::Fatal: prefix = "(FATAL): "; break;
            default: break;
        }
        const char* name = category ? category->name : "";
        const bool showCategory = category && std::strcmp(name, "engine") != 0 && std::strcmp(name, "editor") != 0 &&
                                  std::strcmp(name, "lua") != 0 && std::strcmp(name, "build") != 0;
        if (showCategory) {
            std::fprintf(stderr, "%s[%s] %.*s\n", prefix, name, static_cast<int>(text.size()), text.data());
        } else {
            std::fprintf(stderr, "%s%.*s\n", prefix, static_cast<int>(text.size()), text.data());
        }
    }

    // ---- gates (queueMutex held)

    void recomputeGatesLocked(LoggerState& s) {
        const size_t count = s.categoryCount.load(std::memory_order_acquire);
        const bool live = s.initialized.load(std::memory_order_relaxed);
        for (size_t c = 0; c < count; c++) {
            CategorySlot& slot = s.categories[c];
            uint8_t lowestSink = static_cast<uint8_t>(LogLevel::Off);
            for (size_t i = 0; i < s.sinkCount; i++) {
                lowestSink = std::min(lowestSink, s.sinks[i]->levelFor(slot.category));
            }
            const uint8_t own = slot.level.load(std::memory_order_relaxed);
            // before init the stderr fallback takes everything the category lets through
            const uint8_t effective = live ? std::max(own, lowestSink) : own;
            slot.category.effective.store(effective, std::memory_order_relaxed);
        }
    }

    void logRecomputeGates() {
        LoggerState& s = logState();
        std::lock_guard<std::mutex> lock(s.queueMutex);
        recomputeGatesLocked(s);
    }

    // ---- the record a sink sees

    struct PendingRecord {
        LogRecord record;
        uint32_t excludedSink = 0;
    };

    void dispatchToSink(SinkEntry& entry, const LogRecord& record) {
        ThreadState& thread = logThreadState();
        const uint32_t previous = thread.currentSink;
        thread.currentSink = entry.id;
        entry.sink->write(record);
        thread.currentSink = previous;
    }

    // an Inline sink written under the lock: its afterWrite() once the lock is released
    void queueAfterWrite(const SinkPtr& entry) {
        std::vector<SinkPtr>& pending = logThreadState().afterWrite;
        for (const SinkPtr& queued : pending) {
            if (queued == entry) return;
        }
        pending.push_back(entry);
    }

    // The afterWrite() of the sinks written on this thread, the logger's lock released. One
    // that logs queues its sink again and runs it from here (no recursion past one level).
    void runAfterWrite() {
        ThreadState& thread = logThreadState();
        if (thread.holdsQueueLock) return;
        while (!thread.afterWrite.empty()) {
            std::vector<SinkPtr> running;
            running.swap(thread.afterWrite);
            for (const SinkPtr& entry : running) {
                entry->sink->afterWrite();
            }
            running.clear();
            // the list's storage back for the next record (no allocation per record)
            if (thread.afterWrite.empty()) thread.afterWrite.swap(running);
        }
    }

    // ---- the queue (queueMutex held)

    // Room for size bytes at the tail, wrapping to the start when the end is too short.
    // Returns the offset, or SIZE_MAX when the queue is too full.
    size_t reserveLocked(LoggerState& s, size_t size) {
        if (size > s.capacity) return SIZE_MAX;
        if (s.used == 0) {
            s.head = 0;
            s.tail = 0;
        }
        if (s.tail >= s.head && !(s.used == s.capacity)) {
            const size_t atEnd = s.capacity - s.tail;
            if (size <= atEnd) {
                return s.tail;
            }
            // the end is too short: pad it and start over at 0, if the front has room
            if (size <= s.head) {
                if (atEnd >= sizeof(EntryHeader)) {
                    EntryHeader padding{};
                    padding.size = static_cast<uint32_t>(atEnd);
                    padding.kind = kEntryPadding;
                    std::memcpy(s.ring.get() + s.tail, &padding, sizeof(padding));
                }
                s.used += atEnd;
                s.tail = 0;
                return 0;
            }
            return SIZE_MAX;
        }
        if (s.tail < s.head && size <= s.head - s.tail) {
            return s.tail;
        }
        return SIZE_MAX;
    }

    void commitLocked(LoggerState& s, size_t offset, size_t size) {
        s.tail = offset + size;
        if (s.tail == s.capacity) s.tail = 0;
        s.used += size;
        s.highWater = std::max(s.highWater, s.used);
    }

    size_t entrySize(const LogRecord& record) {
        return logAlignUp(sizeof(EntryHeader) + record.frameCount * sizeof(void*) + record.file.size() +
                       record.function.size() + record.threadName.size() + record.text.size());
    }

    void writeEntry(uint8_t* at, size_t size, const LogRecord& record, uint32_t excludedSink) {
        EntryHeader header{};
        header.size = static_cast<uint32_t>(size);
        header.kind = kEntryRecord;
        header.level = static_cast<uint8_t>(record.level);
        header.category = record.category->index;
        header.frameCount = static_cast<uint8_t>(record.frameCount);
        header.flags = record.flags;
        header.line = record.line;
        header.textLength = static_cast<uint32_t>(record.text.size());
        header.fileLength = static_cast<uint16_t>(record.file.size());
        header.functionLength = static_cast<uint16_t>(record.function.size());
        header.threadNameLength = static_cast<uint8_t>(record.threadName.size());
        header.excludedSink = excludedSink;
        header.id = record.id;
        header.wallTime = record.wallTime;
        header.frame = record.frame;
        header.threadId = record.threadId;
        std::memcpy(at, &header, sizeof(header));
        uint8_t* cursor = at + sizeof(header);
        if (record.frameCount) {
            std::memcpy(cursor, record.frames, record.frameCount * sizeof(void*));
            cursor += record.frameCount * sizeof(void*);
        }
        auto put = [&cursor](std::string_view text) {
            if (!text.empty()) std::memcpy(cursor, text.data(), text.size());
            cursor += text.size();
        };
        put(record.file);
        put(record.function);
        put(record.threadName);
        put(record.text);
    }

    void readEntry(const uint8_t* at, LogRecord& record, uint32_t& excludedSink) {
        EntryHeader header;
        std::memcpy(&header, at, sizeof(header));
        LoggerState& s = logState();
        record.id = header.id;
        record.level = static_cast<LogLevel>(header.level);
        record.category = &s.categories[header.category].category;
        record.flags = header.flags;
        record.wallTime = header.wallTime;
        record.frame = header.frame;
        record.threadId = header.threadId;
        record.line = header.line;
        const uint8_t* cursor = at + sizeof(header);
        record.frameCount = header.frameCount;
        record.frames = header.frameCount ? reinterpret_cast<const void* const*>(cursor) : nullptr;
        cursor += header.frameCount * sizeof(void*);
        auto take = [&cursor](size_t length) {
            std::string_view view(reinterpret_cast<const char*>(cursor), length);
            cursor += length;
            return view;
        };
        record.file = take(header.fileLength);
        record.function = take(header.functionLength);
        record.threadName = take(header.threadNameLength);
        record.text = take(header.textLength);
        excludedSink = header.excludedSink;
    }

    // ---- delivery (queueMutex held): inline sinks now, async ones through the queue

    void deliverLocked(LoggerState& s, std::unique_lock<std::mutex>* lock, LogRecord& record, bool mayWait);

    void reportDropsLocked(LoggerState& s) {
        const uint64_t dropped = s.dropped.load(std::memory_order_relaxed);
        if (dropped == s.droppedReported) return;
        const uint64_t lost = dropped - s.droppedReported;
        char text[96];
        const int length = std::snprintf(text, sizeof(text), "[log] dropped %llu messages (queue full)",
                                         static_cast<unsigned long long>(lost));
        LogRecord notice;
        notice.level = LogLevel::Warn;
        notice.category = &logCategoryEngine();
        notice.flags = LogFlagInternal | LogFlagNoBacktrace;
        notice.text = std::string_view(text, static_cast<size_t>(std::max(0, length)));
        ThreadState& thread = logCurrentThread();
        notice.threadId = thread.id;
        notice.threadName = std::string_view(thread.name, std::strlen(thread.name));
        notice.wallTime = logWallSeconds();
        notice.frame = s.frame.load(std::memory_order_relaxed);
        // only once there is room again, so the notice itself is not lost
        if (reserveLocked(s, entrySize(notice)) == SIZE_MAX) return;
        s.droppedReported = dropped;
        deliverLocked(s, nullptr, notice, false);
    }

    void deliverLocked(LoggerState& s, std::unique_lock<std::mutex>* lock, LogRecord& record, bool mayWait) {
        ThreadState& thread = logThreadState();
        const uint32_t excluded = thread.currentSink;
        const bool async = s.async.load(std::memory_order_relaxed);

        // Fatal reaches the Async sinks synchronously (deliverFatal), everything else through the queue
        bool queue = false;
        if (async && record.level != LogLevel::Fatal) {
            for (size_t i = 0; i < s.sinkCount && !queue; i++) {
                const SinkEntry& entry = *s.sinks[i];
                queue = !entry.inlineDelivery && entry.id != excluded && entry.accepts(record.level, *record.category);
            }
        }
        // Room first, the id after: records enter the queue in id order even when one waited
        size_t size = 0;
        size_t offset = SIZE_MAX;
        if (queue) {
            size = entrySize(record);
            offset = reserveLocked(s, size);
            if (offset == SIZE_MAX && mayWait && lock && record.level >= LogLevel::Warn) {
                // a warning or worse waits a little for the drain thread
                const auto deadline = std::chrono::steady_clock::now() + kQueueFullWait;
                if (s.drainSleeping) s.drainCv.notify_one();
                thread.holdsQueueLock = false;
                while (offset == SIZE_MAX && !s.stopping) {
                    const auto remaining = deadline - std::chrono::steady_clock::now();
                    if (remaining > std::chrono::steady_clock::duration::zero()) {
                        jasperengine::jobs::detail::timedWait(s.spaceCv, *lock, remaining);
                    }
                    const bool timedOut = std::chrono::steady_clock::now() >= deadline;
                    offset = reserveLocked(s, size);
                    if (timedOut) break;
                }
                thread.holdsQueueLock = true;
            }
            if (offset == SIZE_MAX) {
                s.dropped.fetch_add(1, std::memory_order_relaxed);
                if (s.drainSleeping) s.drainCv.notify_one();
                queue = false;
            }
        }

        record.id = s.lastId.fetch_add(1, std::memory_order_relaxed) + 1;
        if (queue) {
            // in before the Inline sinks run: one of them logging puts its record after this one
            writeEntry(s.ring.get() + offset, size, record, excluded);
            commitLocked(s, offset, size);
            s.pushedSeq.fetch_add(1, std::memory_order_release);
            // Info and below ride the drain thread's next round; warnings and a filling queue wake it
            if (s.drainSleeping && (record.level >= LogLevel::Warn || s.used > s.capacity / 4)) {
                s.drainCv.notify_one();
            }
        }
        for (size_t i = 0; i < s.sinkCount; i++) {
            SinkEntry& entry = *s.sinks[i];
            if ((entry.inlineDelivery || !async) && entry.id != excluded && entry.accepts(record.level, *record.category)) {
                dispatchToSink(entry, record);
                if (entry.afterWrite) queueAfterWrite(s.sinks[i]);
            }
        }
        if (queue && s.dropped.load(std::memory_order_relaxed) != s.droppedReported) {
            reportDropsLocked(s);
        }
    }

    // ---- the rate limit

    void emitSummaries(const ValveSummary* summaries, size_t count);

    // False when the record is held back. Expired windows with suppressed records are
    // collected into summaries (emitted by the caller, outside the valve lock).
    bool valvePass(LoggerState& s, uint64_t key, LogLevel level, const LogCategory& category, std::string_view sample,
                   int64_t nowMs, ValveSummary* summaries, size_t& summaryCount) {
        const size_t stripe = static_cast<size_t>((key >> 8) % kValveStripes);
        std::lock_guard<std::mutex> lock(s.valveMutexes[stripe]);
        const size_t base = stripe * kValveStripeSlots;
        const size_t start = static_cast<size_t>(key % kValveStripeSlots);
        ValveSlot* slot = nullptr;
        ValveSlot* victim = nullptr;
        for (size_t probe = 0; probe < kValveProbe; probe++) {
            ValveSlot& candidate = s.valve[base + (start + probe) % kValveStripeSlots];
            if (candidate.key == key) {
                slot = &candidate;
                break;
            }
            // a free slot, else the one whose window started first
            if (candidate.key == 0) {
                if (!victim || victim->key != 0) victim = &candidate;
            } else if (!victim || (victim->key != 0 && candidate.windowStartMs < victim->windowStartMs)) {
                victim = &candidate;
            }
        }
        if (!slot) {
            slot = victim;
            if (slot->key != 0 && slot->suppressed > 0 && summaryCount < 2) {
                ValveSummary& summary = summaries[summaryCount++];
                summary.suppressed = slot->suppressed;
                summary.level = slot->level;
                summary.category = slot->category;
                std::memcpy(summary.sample, slot->sample, sizeof(summary.sample));
            }
            slot->key = key;
            slot->windowStartMs = nowMs;
            slot->count = 0;
            slot->suppressed = 0;
            slot->level = static_cast<uint8_t>(level);
            slot->category = category.index;
            slot->sample[0] = '\0';
        }
        if (nowMs - slot->windowStartMs >= kValveWindowMs) {
            if (slot->suppressed > 0 && summaryCount < 2) {
                ValveSummary& summary = summaries[summaryCount++];
                summary.suppressed = slot->suppressed;
                summary.level = slot->level;
                summary.category = slot->category;
                std::memcpy(summary.sample, slot->sample, sizeof(summary.sample));
            }
            slot->windowStartMs = nowMs;
            slot->count = 0;
            slot->suppressed = 0;
        }
        slot->count++;
        if (slot->count <= kValveLimit) {
            if (slot->count == 1) {
                size_t length = std::min(sample.size(), kValveSampleBytes);
                // between characters, so the summary quoting it is UTF-8
                while (length > 0 && length < sample.size() && (static_cast<unsigned char>(sample[length]) & 0xC0) == 0x80) length--;
                std::memcpy(slot->sample, sample.data(), length);
                slot->sample[length] = '\0';
            }
            return true;
        }
        slot->suppressed++;
        s.suppressed.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // The summaries of windows that ended with records held back
    // (every window with `all`: over or not. How many summaries went out: the rest are for the next)
    size_t sweepValve(LoggerState& s, int64_t nowMs, bool all = false) {
        ValveSummary summaries[16];
        size_t count = 0;
        for (size_t stripe = 0; stripe < kValveStripes && count < 16; stripe++) {
            std::lock_guard<std::mutex> lock(s.valveMutexes[stripe]);
            for (size_t i = 0; i < kValveStripeSlots; i++) {
                ValveSlot& slot = s.valve[stripe * kValveStripeSlots + i];
                if (slot.key == 0 || (!all && nowMs - slot.windowStartMs < kValveWindowMs)) continue;
                if (slot.suppressed > 0) {
                    if (count == 16) break;
                    ValveSummary& summary = summaries[count++];
                    summary.suppressed = slot.suppressed;
                    summary.level = slot.level;
                    summary.category = slot.category;
                    std::memcpy(summary.sample, slot.sample, sizeof(summary.sample));
                }
                slot = ValveSlot{};
            }
        }
        s.nextValveSweepMs.store(nowMs + kValveWindowMs / 4, std::memory_order_relaxed);
        emitSummaries(summaries, count);
        return count;
    }

    // ---- the drain thread

    void drainLoop() {
        LoggerState& s = logState();
        ThreadState& thread = logCurrentThread();
        thread.isDrainThread = true;
        crashDrainThread = true;
        std::snprintf(thread.name, sizeof(thread.name), "je-log");
        logSetOsThreadName("je-log");
        JE_PROFILE_THREAD("je-log");

        std::array<SinkPtr, kMaxSinks> sinks;
        size_t sinkCount = 0;
        uint64_t generation = ~0ull;

        std::unique_lock<std::mutex> lock(s.queueMutex);
        while (true) {
            while (s.used == 0 && !s.stopping) {
                s.drainSleeping = true;
                jasperengine::jobs::detail::timedWait(s.drainCv, lock, kDrainInterval);
                s.drainSleeping = false;
                if (s.used == 0) {
                    lock.unlock();
                    const int64_t now = logMonotonicMs();
                    if (now >= s.nextValveSweepMs.load(std::memory_order_relaxed)) sweepValve(s, now);
                    lock.lock();
                }
            }
            if (s.used == 0 && s.stopping) break;

            if (generation != s.sinkGeneration) {
                generation = s.sinkGeneration;
                sinkCount = s.sinkCount;
                for (size_t i = 0; i < kMaxSinks; i++) sinks[i] = i < sinkCount ? s.sinks[i] : nullptr;
            }
            const size_t start = s.head;
            const size_t bytes = s.used;
            const uint64_t batchEnd = s.pushedSeq.load(std::memory_order_acquire);
            lock.unlock();

            // the batch, sink by sink: each sink's lock is taken once for all of it
            for (size_t i = 0; i < sinkCount; i++) {
                SinkEntry& entry = *sinks[i];
                if (entry.inlineDelivery) continue;
                std::lock_guard<std::mutex> sinkLock(entry.writeMutex);
                size_t position = start;
                size_t consumed = 0;
                while (consumed < bytes) {
                    const size_t atEnd = s.capacity - position;
                    if (atEnd < sizeof(EntryHeader)) {
                        consumed += atEnd;
                        position = 0;
                        continue;
                    }
                    EntryHeader header;
                    std::memcpy(&header, s.ring.get() + position, sizeof(header));
                    if (header.kind == kEntryRecord) {
                        LogRecord record;
                        uint32_t excluded = 0;
                        readEntry(s.ring.get() + position, record, excluded);
                        if (excluded != entry.id && entry.accepts(record.level, *record.category)) {
                            dispatchToSink(entry, record);
                        }
                    }
                    consumed += header.size;
                    position += header.size;
                    if (position >= s.capacity) position = 0;
                }
                entry.sink->flush();
            }

            lock.lock();
            s.used -= bytes;
            s.head = s.used == 0 ? 0 : (start + bytes) % s.capacity;
            if (s.used == 0) s.tail = 0;
            s.drainedSeq.store(batchEnd, std::memory_order_release);
            s.spaceCv.notify_all();
            s.drainedCv.notify_all();
            if (s.dropped.load(std::memory_order_relaxed) != s.droppedReported) {
                thread.holdsQueueLock = true;
                reportDropsLocked(s);
                thread.holdsQueueLock = false;
                if (!thread.afterWrite.empty()) {
                    lock.unlock();
                    runAfterWrite();
                    lock.lock();
                }
            }
        }
        s.drainedSeq.store(s.pushedSeq.load(std::memory_order_acquire), std::memory_order_release);
        s.drainedCv.notify_all();
    }

    // ---- one record

#if defined(__GNUC__) || defined(__clang__)
    #define JE_LOGGER_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
    #define JE_LOGGER_NOINLINE __declspec(noinline)
#else
    #define JE_LOGGER_NOINLINE
#endif

    JE_LOGGER_NOINLINE bool captureBacktrace(LoggerState& s, LogRecord& record, ThreadState& thread) {
        const int64_t now = logMonotonicMs();
        {
            std::lock_guard<std::mutex> lock(s.backtraceMutex);
            if (now - s.backtraceWindowMs >= 1000) {
                s.backtraceWindowMs = now;
                s.backtraceCount = 0;
            }
            if (s.backtraceCount >= kBacktracesPerSecond) {
                s.backtracesOmitted.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            s.backtraceCount++;
        }
        // from the caller of this function (submit, then the Logger entry point and the call site)
        record.frameCount = static_cast<uint32_t>(Backtrace::capture(thread.frames, LogRecord::MAX_FRAMES, 1));
        record.frames = thread.frames;
        return record.frameCount > 0;
    }

    // Locks `mutex` by `deadline` (false: it did not). A try_lock loop, not std::timed_mutex:
    // libstdc++'s try_lock_until locks through pthread_mutex_clocklock, which GCC 11's
    // ThreadSanitizer does not intercept (io/watch/backends/Backends.h, lockWithin)
    bool lockUntil(std::mutex& mutex, std::chrono::steady_clock::time_point deadline) {
        while (!mutex.try_lock()) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        return true;
    }

    void deliverFatal(LoggerState& s, const LogRecord& record) {
        // what was queued before it goes first, then the record to every Async sink from here
        if (s.async.load(std::memory_order_relaxed)) {
            Logger::flushForCrash(std::chrono::milliseconds(500));
        }
        std::array<SinkPtr, kMaxSinks> sinks;
        size_t sinkCount = 0;
        {
            std::lock_guard<std::mutex> lock(s.queueMutex);
            sinkCount = s.sinkCount;
            for (size_t i = 0; i < sinkCount; i++) sinks[i] = s.sinks[i];
        }
        size_t ownSinks = 0;
        for (size_t i = 0; i < sinkCount; i++) ownSinks += sinks[i]->resident ? 0 : 1;
        if (ownSinks == 0) {
            writeFallback(record.level, record.category, record.text);
            return;
        }
        if (!s.async.load(std::memory_order_relaxed)) return;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        const uint32_t current = logThreadState().currentSink;
        for (size_t i = 0; i < sinkCount; i++) {
            SinkEntry& entry = *sinks[i];
            // the sink this thread is inside of (a Fatal from a sink) holds its own lock
            if (entry.inlineDelivery || entry.id == current || !entry.accepts(record.level, *record.category)) continue;
            if (!lockUntil(entry.writeMutex, deadline)) continue;
            dispatchToSink(entry, record);
            entry.sink->flush();
            entry.writeMutex.unlock();
        }
    }

    // Before init() and after shutdown(): the resident sinks get the record on this thread
    // (with an id, as after init), then the stderr fallback writes it
    void deliverResident(LoggerState& s, ThreadState& thread, LogLevel level, const LogCategory& category,
                         const LogSource* source, std::string_view text, uint32_t flags) {
        LogRecord record;
        record.level = level;
        record.category = &category;
        record.flags = flags;
        record.wallTime = logWallSeconds();
        record.frame = s.frame.load(std::memory_order_relaxed);
        record.threadId = thread.id;
        record.threadName = std::string_view(thread.name, std::strlen(thread.name));
        if (source) {
            if (source->file) {
                std::string_view file(source->file);
                if (file.size() > LogRecord::MAX_FILE_BYTES) file = file.substr(file.size() - LogRecord::MAX_FILE_BYTES);
                record.file = file;
            }
            if (source->function) record.function = std::string_view(source->function).substr(0, LogRecord::MAX_FUNCTION_BYTES);
            record.line = source->line;
        }
        if (text.size() > LogRecord::MAX_TEXT_BYTES) {
            text = text.substr(0, LogRecord::MAX_TEXT_BYTES);
            record.flags |= LogFlagTruncated;
        }
        record.text = text;
        std::lock_guard<std::mutex> lock(s.queueMutex);
        // init() ran meanwhile: the caller's fallback line still goes out, the sinks have it
        record.id = s.lastId.fetch_add(1, std::memory_order_relaxed) + 1;
        thread.holdsQueueLock = true;
        for (size_t i = 0; i < s.sinkCount; i++) {
            SinkEntry& entry = *s.sinks[i];
            if (!entry.resident || entry.id == thread.currentSink || !entry.accepts(level, category)) continue;
            dispatchToSink(entry, record);
            if (entry.afterWrite) queueAfterWrite(s.sinks[i]);
        }
        thread.holdsQueueLock = false;
    }

    void logSubmit(LogLevel level, const LogCategory& category, const LogSource* source, std::string_view text,
                uint32_t flags, uint64_t valveKey) {
        LoggerState& s = logState();
        ThreadState& thread = logCurrentThread();

        // every sink gets UTF-8: a child's output in another code page, a cut string or, on
        // Windows, ANSI text (StringUtils::legacyToUtf8); valid text costs one scan. A local, not
        // the thread's buffer: the summaries this call emits come back into logSubmit.
        std::string repaired;
        if (!StringUtils::isValidUtf8(text)) {
            repaired = StringUtils::legacyToUtf8(text);
            text = repaired;
        }

        if (!s.initialized.load(std::memory_order_acquire)) {
            if (s.residentCount.load(std::memory_order_acquire) > 0 && !thread.holdsQueueLock) {
                deliverResident(s, thread, level, category, source, text, flags);
            }
            writeFallback(level, &category, text);
            runAfterWrite();
            return;
        }

        const int64_t nowMs = logMonotonicMs();
        ValveSummary summaries[2];
        size_t summaryCount = 0;
        bool pass = true;
        if (!(flags & (LogFlagInternal | LogFlagNoRateLimit)) && level != LogLevel::Fatal) {
            uint64_t key = valveKey ^ (static_cast<uint64_t>(level) << 56);
            if (key == 0) key = 1;   // 0 marks a free slot
            pass = valvePass(s, key, level, category, text, nowMs, summaries, summaryCount);
        }
        if (summaryCount) emitSummaries(summaries, summaryCount);
        if (!s.async.load(std::memory_order_relaxed) && nowMs >= s.nextValveSweepMs.load(std::memory_order_relaxed)) {
            sweepValve(s, nowMs);
        }
        if (!pass) return;

        LogRecord record;
        record.level = level;
        record.category = &category;
        record.flags = flags;
        record.wallTime = logWallSeconds();
        record.frame = s.frame.load(std::memory_order_relaxed);
        record.threadId = thread.id;
        record.threadName = std::string_view(thread.name, std::strlen(thread.name));
        if (source) {
            if (source->file) {
                std::string_view file(source->file);
                if (file.size() > LogRecord::MAX_FILE_BYTES) file = file.substr(file.size() - LogRecord::MAX_FILE_BYTES);
                record.file = file;
            }
            if (source->function) {
                record.function = std::string_view(source->function).substr(0, LogRecord::MAX_FUNCTION_BYTES);
            }
            record.line = source->line;
        }

        std::string& large = thread.largeText;
        if (text.size() > LogRecord::MAX_TEXT_BYTES) {
            char suffix[48];
            size_t keep = LogRecord::MAX_TEXT_BYTES - 40;
            // between characters, never inside one
            while (keep > 0 && (static_cast<unsigned char>(text[keep]) & 0xC0) == 0x80) keep--;
            const int suffixLength = std::snprintf(suffix, sizeof(suffix), "\xE2\x80\xA6[truncated %llu bytes]",
                                                   static_cast<unsigned long long>(text.size() - keep));
            if (text.data() != large.data()) {
                large.assign(text.data(), keep);
            } else {
                large.resize(keep);
            }
            large.append(suffix, static_cast<size_t>(std::max(0, suffixLength)));
            text = large;
            record.flags |= LogFlagTruncated;
        }
        record.text = text;

        if (level >= LogLevel::Error && !(flags & LogFlagNoBacktrace) && s.backtraceOnError.load(std::memory_order_relaxed)) {
            captureBacktrace(s, record, thread);
        }

        if (thread.holdsQueueLock) {
            // logged from inside an Inline sink: no second lock, no waiting
            deliverLocked(s, nullptr, record, false);
        } else {
            // a short spin before sleeping on the lock: producers hold it for a copy only
            std::unique_lock<std::mutex> lock(s.queueMutex, std::defer_lock);
            for (int spin = 0; spin < 200 && !lock.try_lock(); spin++) {
                logCpuRelax();
            }
            if (!lock.owns_lock()) lock.lock();
            thread.holdsQueueLock = true;
            // a record logged from inside a sink on the drain thread must not wait for itself
            deliverLocked(s, &lock, record, !thread.isDrainThread);
            thread.holdsQueueLock = false;
        }

        if (level == LogLevel::Fatal) {
            deliverFatal(s, record);
        }
        // outside the lock: the sinks' afterWrite (from inside an Inline sink, the outer call's)
        if (!thread.holdsQueueLock && !thread.afterWrite.empty()) {
            runAfterWrite();
        }
    }

    void emitSummaries(const ValveSummary* summaries, size_t count) {
        LoggerState& s = logState();
        for (size_t i = 0; i < count; i++) {
            char text[160];
            const int length = std::snprintf(text, sizeof(text), "suppressed %u similar messages: \"%s\"",
                                             summaries[i].suppressed, summaries[i].sample);
            const LogCategory& category = s.categories[summaries[i].category].category;
            logSubmit(static_cast<LogLevel>(summaries[i].level), category, nullptr,
                   std::string_view(text, static_cast<size_t>(std::max(0, std::min<int>(length, sizeof(text) - 1)))),
                   LogFlagInternal | LogFlagNoBacktrace, 0);
        }
    }

    LogCategory& logBuiltIn(const char* name) {
        return Logger::registerCategory(name, LogLevel::Info);
    }

}

// ------------------------------------------------------------ categories

const char* jasperengine::logLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Debug: return "debug";
        case LogLevel::Info:  return "info";
        case LogLevel::Warn:  return "warn";
        case LogLevel::Error: return "error";
        case LogLevel::Fatal: return "fatal";
        case LogLevel::Off:   return "off";
    }
    return "info";
}

bool jasperengine::parseLogLevel(const char* name, LogLevel& out) {
    if (!name) return false;
    char lower[16] = {0};
    size_t i = 0;
    for (; name[i] && i + 1 < sizeof(lower); i++) {
        const char c = name[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    if (name[i]) return false;
    struct Name { const char* text; LogLevel level; };
    static const Name names[] = {
        {"trace", LogLevel::Trace}, {"verbose", LogLevel::Trace},
        {"debug", LogLevel::Debug},
        {"info", LogLevel::Info}, {"information", LogLevel::Info}, {"notice", LogLevel::Info}, {"log", LogLevel::Info},
        {"warn", LogLevel::Warn}, {"warning", LogLevel::Warn},
        {"error", LogLevel::Error}, {"err", LogLevel::Error},
        {"fatal", LogLevel::Fatal}, {"critical", LogLevel::Fatal}, {"alert", LogLevel::Fatal}, {"emergency", LogLevel::Fatal},
        {"off", LogLevel::Off}, {"none", LogLevel::Off},
    };
    for (const Name& entry : names) {
        if (std::strcmp(lower, entry.text) == 0) {
            out = entry.level;
            return true;
        }
    }
    return false;
}

LogCategory& jasperengine::logCategoryEngine() { static LogCategory& c = logBuiltIn("engine"); return c; }
LogCategory& jasperengine::logCategoryEditor() { static LogCategory& c = logBuiltIn("editor"); return c; }
LogCategory& jasperengine::logCategoryBuild() { static LogCategory& c = logBuiltIn("build"); return c; }
LogCategory& jasperengine::logCategoryLua() { static LogCategory& c = logBuiltIn("lua"); return c; }
LogCategory& jasperengine::logCategoryScript() { static LogCategory& c = logBuiltIn("script"); return c; }
LogCategory& jasperengine::logCategoryRender() { static LogCategory& c = logBuiltIn("render"); return c; }
LogCategory& jasperengine::logCategoryShader() { static LogCategory& c = logBuiltIn("shader"); return c; }
LogCategory& jasperengine::logCategoryPhysics() { static LogCategory& c = logBuiltIn("physics"); return c; }
LogCategory& jasperengine::logCategoryAudio() { static LogCategory& c = logBuiltIn("audio"); return c; }
LogCategory& jasperengine::logCategoryAsset() { static LogCategory& c = logBuiltIn("asset"); return c; }
LogCategory& jasperengine::logCategoryHtmlUI() { static LogCategory& c = logBuiltIn("htmlui"); return c; }
LogCategory& jasperengine::logCategoryIO() { static LogCategory& c = logBuiltIn("io"); return c; }
LogCategory& jasperengine::logCategoryMcp() { static LogCategory& c = logBuiltIn("mcp"); return c; }

namespace je_logger {
    // the built-ins in their documented order, before any other category
    void logRegisterBuiltIns() {
        static const bool registered = []() {
            logCategoryEngine(); logCategoryEditor(); logCategoryBuild(); logCategoryLua(); logCategoryScript();
            logCategoryRender(); logCategoryShader(); logCategoryPhysics(); logCategoryAudio(); logCategoryAsset();
            logCategoryHtmlUI(); logCategoryIO(); logCategoryMcp();
            return true;
        }();
        (void)registered;
    }
}

LogCategory& Logger::registerCategory(std::string_view name, LogLevel defaultLevel) {
    LoggerState& s = logState();
    LogCategory* found = nullptr;
    bool full = false;
    {
        std::lock_guard<std::mutex> lock(s.categoryMutex);
        const size_t count = s.categoryCount.load(std::memory_order_relaxed);
        for (size_t i = 0; i < count; i++) {
            if (name == s.categories[i].name) return s.categories[i].category;
        }
        if (count < kMaxCategories && !name.empty()) {
            CategorySlot& slot = s.categories[count];
            const size_t length = std::min(name.size(), sizeof(slot.name) - 1);
            std::memcpy(slot.name, name.data(), length);
            slot.name[length] = '\0';
            slot.category.name = slot.name;
            slot.category.index = static_cast<uint8_t>(count);
            // a level set for everything (--log-level) applies to categories made later
            const uint8_t level = std::max(static_cast<uint8_t>(defaultLevel), s.defaultLevel.load(std::memory_order_relaxed));
            slot.level.store(level, std::memory_order_relaxed);
            slot.category.effective.store(level, std::memory_order_relaxed);
            s.categoryCount.store(count + 1, std::memory_order_release);
            found = &slot.category;
        } else {
            full = true;
        }
    }
    if (full) {
        JE_LOG_ONCE(Warn, logCategoryEngine(), "Too many log categories (at most %zu): '%.*s' logs as 'engine'",
                    kMaxCategories, static_cast<int>(name.size()), name.data());
        return logCategoryEngine();
    }
    // a category first used while this thread delivers a record (holding the queue lock)
    if (logThreadState().holdsQueueLock) {
        recomputeGatesLocked(s);
    } else {
        logRecomputeGates();
    }
    return *found;
}

LogCategory* Logger::findCategory(std::string_view name) {
    logRegisterBuiltIns();
    LoggerState& s = logState();
    const size_t count = s.categoryCount.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; i++) {
        if (name == s.categories[i].name) return &s.categories[i].category;
    }
    return nullptr;
}

size_t Logger::categoryCount() {
    logRegisterBuiltIns();
    return logState().categoryCount.load(std::memory_order_acquire);
}

LogCategory& Logger::categoryAt(size_t index) {
    logRegisterBuiltIns();
    LoggerState& s = logState();
    if (index >= s.categoryCount.load(std::memory_order_acquire)) return logCategoryEngine();
    return s.categories[index].category;
}

void Logger::setCategoryLevel(LogCategory& category, LogLevel level) {
    LoggerState& s = logState();
    s.categories[category.index].level.store(static_cast<uint8_t>(level), std::memory_order_relaxed);
    logRecomputeGates();
}

LogLevel Logger::categoryLevel(const LogCategory& category) {
    return static_cast<LogLevel>(logState().categories[category.index].level.load(std::memory_order_relaxed));
}

void Logger::setDefaultLevel(LogLevel level) {
    logRegisterBuiltIns();
    LoggerState& s = logState();
    s.defaultLevel.store(static_cast<uint8_t>(level), std::memory_order_relaxed);
    const size_t count = s.categoryCount.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; i++) {
        s.categories[i].level.store(static_cast<uint8_t>(level), std::memory_order_relaxed);
    }
    logRecomputeGates();
}

LogLevel Logger::defaultLevel() {
    return static_cast<LogLevel>(logState().defaultLevel.load(std::memory_order_relaxed));
}

bool Logger::applyLevelSpec(std::string_view spec, std::string* error) {
    struct Item { std::string category; LogLevel level; };
    std::vector<Item> items;
    bool hasDefault = false;
    LogLevel defaultValue = LogLevel::Info;
    size_t start = 0;
    while (start <= spec.size()) {
        size_t end = spec.find(',', start);
        if (end == std::string_view::npos) end = spec.size();
        std::string_view item = spec.substr(start, end - start);
        while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
        while (!item.empty() && item.back() == ' ') item.remove_suffix(1);
        if (!item.empty()) {
            const size_t equals = item.find('=');
            const std::string levelText(equals == std::string_view::npos ? item : item.substr(equals + 1));
            LogLevel level;
            if (!parseLogLevel(levelText.c_str(), level)) {
                if (error) *error = "unknown log level '" + levelText + "' (trace, debug, info, warn, error, fatal, off)";
                return false;
            }
            if (equals == std::string_view::npos) {
                if (hasDefault) {
                    if (error) *error = "more than one level without a category in '" + std::string(spec) + "'";
                    return false;
                }
                hasDefault = true;
                defaultValue = level;
            } else {
                const std::string_view name = item.substr(0, equals);
                if (name.empty()) {
                    if (error) *error = "a category name is missing in '" + std::string(item) + "'";
                    return false;
                }
                items.push_back({std::string(name), level});
            }
        }
        start = end + 1;
    }
    if (hasDefault) setDefaultLevel(defaultValue);
    for (const Item& item : items) {
        setCategoryLevel(registerCategory(item.category, item.level), item.level);
    }
    return true;
}

// ------------------------------------------------------------ lifecycle

void Logger::init(const Config& config) {
    logRegisterBuiltIns();
    LoggerState& s = logState();
    {
        std::lock_guard<std::mutex> lock(s.queueMutex);
        if (s.initialized.load(std::memory_order_relaxed)) return;
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
        const bool async = false;
#elif defined(__EMSCRIPTEN__)
        // console calls from a worker are proxied to the main thread anyway
        const bool async = false;
#else
        const bool async = config.asyncDrain;
#endif
        s.capacity = std::max<size_t>(logAlignUp(config.queueBytes), 256 * 1024);
        s.ring.reset(new uint8_t[s.capacity]);
        s.head = s.tail = s.used = s.highWater = 0;
        s.stopping = false;
        s.async.store(async, std::memory_order_relaxed);
        s.backtraceOnError.store(config.backtraceOnError, std::memory_order_relaxed);
        s.initialized.store(true, std::memory_order_release);
        if (async) {
            s.drainThread = new std::thread(drainLoop);
        }
    }
    if (static_cast<uint8_t>(config.defaultLevel) != s.defaultLevel.load(std::memory_order_relaxed)) {
        setDefaultLevel(config.defaultLevel);
    } else {
        logRecomputeGates();
    }
    // a process that exits without shutdown() still writes what was logged
    static const bool exitHook = []() {
        std::atexit([]() { Logger::flushForCrash(std::chrono::milliseconds(500)); });
        return true;
    }();
    (void)exitHook;
}

bool Logger::isInitialized() {
    return logState().initialized.load(std::memory_order_acquire);
}

void Logger::shutdown() {
    LoggerState& s = logState();
    if (!s.initialized.load(std::memory_order_acquire)) return;
    // what the rate limit still holds back is reported while the sinks are there
    sweepValve(s, std::numeric_limits<int64_t>::max() / 2);
    std::thread* drain = nullptr;
    {
        std::lock_guard<std::mutex> lock(s.queueMutex);
        if (!s.initialized.load(std::memory_order_relaxed)) return;
        // records logged from here on go to stderr; the drain thread empties the queue and stops
        s.initialized.store(false, std::memory_order_release);
        s.stopping = true;
        s.drainCv.notify_all();
        s.spaceCv.notify_all();
        drain = s.drainThread;
        s.drainThread = nullptr;
    }
    if (drain) {
        if (drain->get_id() == std::this_thread::get_id()) {
            drain->detach();
        } else {
            drain->join();
        }
        delete drain;
    }
    std::array<SinkPtr, kMaxSinks> sinks;
    size_t sinkCount = 0;
    {
        std::lock_guard<std::mutex> lock(s.queueMutex);
        s.async.store(false, std::memory_order_relaxed);
        // the resident sinks stay (they get what is logged from here on, as before init)
        size_t kept = 0;
        for (size_t i = 0; i < s.sinkCount; i++) {
            if (s.sinks[i]->resident) {
                s.sinks[kept++] = std::move(s.sinks[i]);
            } else {
                sinks[sinkCount++] = std::move(s.sinks[i]);
            }
        }
        for (size_t i = kept; i < s.sinkCount; i++) s.sinks[i] = nullptr;
        s.sinkCount = kept;
        s.sinkGeneration++;
        s.ring.reset();
        s.capacity = s.head = s.tail = s.used = 0;
        recomputeGatesLocked(s);
    }
    for (size_t i = 0; i < sinkCount; i++) {
        std::lock_guard<std::mutex> sinkLock(sinks[i]->writeMutex);
        sinks[i]->sink->flush();
    }
}

// ------------------------------------------------------------ sinks

LogSink::~LogSink() = default;

Logger::SinkId Logger::addSink(std::unique_ptr<LogSink> sink) {
    if (!sink) return 0;
    LoggerState& s = logState();
    auto entry = std::make_shared<SinkEntry>();
    entry->inlineDelivery = sink->delivery() == LogSink::Delivery::Inline;
    entry->resident = entry->inlineDelivery && sink->resident();
    entry->afterWrite = entry->inlineDelivery && sink->wantsAfterWrite();
    entry->sink = std::move(sink);
    std::lock_guard<std::mutex> lock(s.queueMutex);
    if (s.sinkCount == kMaxSinks) return 0;
    entry->id = s.nextSinkId++;
    s.sinks[s.sinkCount++] = entry;
    if (entry->resident) s.residentCount.fetch_add(1, std::memory_order_release);
    s.sinkGeneration++;
    recomputeGatesLocked(s);
    return entry->id;
}

void Logger::removeSink(SinkId id) {
    LoggerState& s = logState();
    SinkPtr removed;
    {
        std::lock_guard<std::mutex> lock(s.queueMutex);
        for (size_t i = 0; i < s.sinkCount; i++) {
            if (s.sinks[i]->id != id) continue;
            removed = std::move(s.sinks[i]);
            if (removed->resident) s.residentCount.fetch_sub(1, std::memory_order_release);
            for (size_t j = i + 1; j < s.sinkCount; j++) s.sinks[j - 1] = std::move(s.sinks[j]);
            s.sinks[--s.sinkCount] = nullptr;
            s.sinkGeneration++;
            recomputeGatesLocked(s);
            break;
        }
    }
    if (removed) {
        // what it was given is written before it goes
        std::lock_guard<std::mutex> sinkLock(removed->writeMutex);
        removed->sink->flush();
    }
}

Logger::SinkId Logger::findSink(std::string_view name) {
    LoggerState& s = logState();
    std::lock_guard<std::mutex> lock(s.queueMutex);
    for (size_t i = 0; i < s.sinkCount; i++) {
        if (name == s.sinks[i]->sink->name()) return s.sinks[i]->id;
    }
    return 0;
}

void Logger::setSinkLevel(SinkId id, LogLevel level, const LogCategory* category) {
    LoggerState& s = logState();
    std::lock_guard<std::mutex> lock(s.queueMutex);
    for (size_t i = 0; i < s.sinkCount; i++) {
        SinkEntry& entry = *s.sinks[i];
        if (entry.id != id) continue;
        if (category) {
            entry.categoryLevels[category->index].store(static_cast<uint8_t>(level), std::memory_order_relaxed);
        } else {
            entry.sink->minLevel.store(level, std::memory_order_relaxed);
        }
        recomputeGatesLocked(s);
        return;
    }
}

void Logger::clearSinkCategoryLevel(SinkId id, const LogCategory& category) {
    LoggerState& s = logState();
    std::lock_guard<std::mutex> lock(s.queueMutex);
    for (size_t i = 0; i < s.sinkCount; i++) {
        SinkEntry& entry = *s.sinks[i];
        if (entry.id != id) continue;
        entry.categoryLevels[category.index].store(kInheritLevel, std::memory_order_relaxed);
        recomputeGatesLocked(s);
        return;
    }
}

LogLevel Logger::sinkLevel(SinkId id, const LogCategory* category) {
    LoggerState& s = logState();
    std::lock_guard<std::mutex> lock(s.queueMutex);
    for (size_t i = 0; i < s.sinkCount; i++) {
        const SinkEntry& entry = *s.sinks[i];
        if (entry.id != id) continue;
        if (category) return static_cast<LogLevel>(entry.levelFor(*category));
        return entry.sink->minLevel.load(std::memory_order_relaxed);
    }
    return LogLevel::Off;
}

std::vector<Logger::SinkInfo> Logger::sinkInfo() {
    LoggerState& s = logState();
    std::vector<SinkInfo> result;
    std::lock_guard<std::mutex> lock(s.queueMutex);
    const size_t categories = s.categoryCount.load(std::memory_order_acquire);
    for (size_t i = 0; i < s.sinkCount; i++) {
        const SinkEntry& entry = *s.sinks[i];
        SinkInfo info;
        info.id = entry.id;
        info.name = entry.sink->name();
        info.delivery = entry.inlineDelivery ? LogSink::Delivery::Inline : LogSink::Delivery::Async;
        info.level = entry.sink->minLevel.load(std::memory_order_relaxed);
        for (size_t c = 0; c < categories; c++) {
            const uint8_t own = entry.categoryLevels[c].load(std::memory_order_relaxed);
            if (own != kInheritLevel) info.categoryLevels.emplace_back(&s.categories[c].category, static_cast<LogLevel>(own));
        }
        result.push_back(std::move(info));
    }
    return result;
}

// ------------------------------------------------------------ logging

void Logger::writef(LogLevel level, const LogCategory& category, const LogSource* source, const char* format, ...) {
    va_list args;
    va_start(args, format);
    vwritef(level, category, source, format, args);
    va_end(args);
}

void Logger::vwritef(LogLevel level, const LogCategory& category, const LogSource* source, const char* format, va_list args) {
    if (!format) return;
    ThreadState& thread = logThreadState();
    va_list sizing;
    va_copy(sizing, args);
    const int length = std::vsnprintf(thread.formatBuffer, sizeof(thread.formatBuffer), format, sizing);
    va_end(sizing);
    std::string_view text;
    if (length < 0) {
        text = std::string_view();
    } else if (static_cast<size_t>(length) < sizeof(thread.formatBuffer)) {
        text = std::string_view(thread.formatBuffer, static_cast<size_t>(length));
    } else {
        // longer than the inline buffer: the thread's big buffer keeps its capacity
        std::string& large = thread.largeText;
        large.resize(static_cast<size_t>(length) + 1);
        std::vsnprintf(&large[0], large.size(), format, args);
        large.resize(static_cast<size_t>(length));
        text = large;
    }
    // the rate limit's key: the call site's format, or the text of a pass-through format
    const uint64_t key = isPassThroughFormat(format) ? logHashText(text)
                                                     : static_cast<uint64_t>(reinterpret_cast<uintptr_t>(format));
    logSubmit(level, category, source, text, 0, key);
}

void Logger::write(LogLevel level, const LogCategory& category, std::string_view text, const LogSource* source,
                   uint32_t flags, const void* rateKey) {
    if (!shouldLog(level, category)) return;
    logSubmit(level, category, source, text, flags,
           rateKey ? static_cast<uint64_t>(reinterpret_cast<uintptr_t>(rateKey)) : logHashText(text));
}

// ------------------------------------------------------------ flushing

void Logger::flush() {
    LoggerState& s = logState();
    ThreadState& thread = logThreadState();
    if (thread.isDrainThread || thread.holdsQueueLock) return;
    if (!s.initialized.load(std::memory_order_acquire)) return;
    // windows of the rate limit that ended report what they held back
    sweepValve(s, logMonotonicMs());
    if (!s.async.load(std::memory_order_relaxed)) {
        std::array<SinkPtr, kMaxSinks> sinks;
        size_t sinkCount = 0;
        {
            std::lock_guard<std::mutex> lock(s.queueMutex);
            sinkCount = s.sinkCount;
            for (size_t i = 0; i < sinkCount; i++) sinks[i] = s.sinks[i];
        }
        for (size_t i = 0; i < sinkCount; i++) sinks[i]->sink->flush();
        return;
    }
    std::unique_lock<std::mutex> lock(s.queueMutex);
    const uint64_t target = s.pushedSeq.load(std::memory_order_acquire);
    s.drainCv.notify_one();
    // bounded, so a sink stuck in a write cannot hang the caller forever
    jasperengine::jobs::detail::timedWait(s.drainedCv, lock, std::chrono::seconds(5), [&s, target]() {
        return s.drainedSeq.load(std::memory_order_acquire) >= target || s.stopping;
    });
}

void Logger::flushSummaries() {
    LoggerState& s = logState();
    if (!s.initialized.load(std::memory_order_acquire)) return;
    while (sweepValve(s, logMonotonicMs(), true) > 0) {}
    flush();
}

bool Logger::flushForCrash(std::chrono::milliseconds timeout, uint64_t* unwritten) noexcept {
    LoggerState& s = logState();
    if (unwritten) *unwritten = 0;
    if (!s.initialized.load(std::memory_order_acquire)) return false;
    const uint64_t target = s.pushedSeq.load(std::memory_order_acquire);
    if (!s.async.load(std::memory_order_relaxed)) return true;
    if (crashDrainThread) {
        if (unwritten) *unwritten = target - std::min(target, s.drainedSeq.load(std::memory_order_acquire));
        return false;
    }
    // no locks, no condition variables: the drain thread wakes on its own within kDrainInterval
    const int64_t deadline = logMonotonicMs() + timeout.count();
    while (true) {
        const uint64_t drained = s.drainedSeq.load(std::memory_order_acquire);
        if (drained >= target) return true;
        if (logMonotonicMs() >= deadline) {
            if (unwritten) *unwritten = target - drained;
            return false;
        }
        logSleepMs(1);
    }
}

// ------------------------------------------------------------ misc

void Logger::setFrame(uint64_t frame) {
    logState().frame.store(frame, std::memory_order_relaxed);
}

void Logger::setThreadName(const char* name) {
    ThreadState& thread = logThreadState();
    thread.named = true;
    std::snprintf(thread.name, sizeof(thread.name), "%s", name ? name : "");
}

void Logger::setBacktraceOnError(bool enabled) {
    logState().backtraceOnError.store(enabled, std::memory_order_relaxed);
}

bool Logger::backtraceOnError() {
    return logState().backtraceOnError.load(std::memory_order_relaxed);
}

Logger::Stats Logger::stats() {
    LoggerState& s = logState();
    Stats result;
    result.records = s.lastId.load(std::memory_order_relaxed);
    result.dropped = s.dropped.load(std::memory_order_relaxed);
    result.suppressed = s.suppressed.load(std::memory_order_relaxed);
    result.backtracesOmitted = s.backtracesOmitted.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(s.queueMutex);
    result.queueCapacity = s.capacity;
    result.queueUsed = s.used;
    result.queueHighWater = s.highWater;
    result.async = s.async.load(std::memory_order_relaxed);
    return result;
}

uint64_t Logger::lastId() {
    return logState().lastId.load(std::memory_order_relaxed);
}

bool Logger::passEvery(std::atomic<int64_t>& next, double seconds) {
    const int64_t now = logMonotonicMs();
    int64_t due = next.load(std::memory_order_relaxed);
    if (now < due) return false;
    const int64_t following = now + static_cast<int64_t>(seconds * 1000.0);
    return next.compare_exchange_strong(due, following, std::memory_order_relaxed);
}
