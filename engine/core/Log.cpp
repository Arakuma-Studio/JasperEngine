// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Log::* on the S09 Logger, and the log's buffer and listeners (feature debug-overlay) as one of
// its sinks: "buffer", Inline and resident (log/LogSink.h). Inline, it sees the records in id
// order on the thread that logs, under the Logger's lock: there it only copies the record into
// the ring (and the pending queue of the main thread's listeners). The listeners run from its
// afterWrite(), on the same thread once the Logger's lock is released, as upstream's Log ran
// them after its own lock. Resident, it gets the records logged before Logger::init and after
// Logger::shutdown too, so the buffer holds lines from the process's first one.

#include "Log.h"
#include "log/Logger.h"
#include "Engine.h"
#include "util/FunctionSubscribe.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <stdarg.h>
#include <stdio.h>
#include <thread>

using namespace jasperengine;

FunctionSubscribe<void(const std::string&, const std::string&, int)> Log::onMessageReceived;

namespace {

    constexpr size_t defaultBufferSize = 500;
    // lines other threads logged that wait for the next frame; past it they are counted, and
    // reported once (as a warning line) when the queue has room again
    constexpr size_t pendingLimit = 1000;
    constexpr size_t levelCount = 7;   // Trace .. Off

    struct Listener {
        LogListenerId id;
        LogDelivery delivery;
        std::function<void(const LogEntry&)> function;
        // removed: a dispatch holding an older list skips it
        std::atomic<bool> removed{false};
        // calls running now, on any thread: removeListener waits for them
        std::atomic<int> calls{0};
    };
    using ListenerList = std::vector<std::shared_ptr<Listener>>;

    // The buffer, the pending queue, the counts and the listener list under one mutex (taken
    // inside the Logger's lock by the sink, alone by everything else); listeners are called
    // from copies of the list, never under it
    struct LogState {
        std::mutex mutex;
        std::deque<LogEntry> buffer;
        size_t bufferSize = defaultBufferSize;
        std::deque<LogEntry> pending;
        size_t pendingDropped = 0;
        uint64_t lastId = 0;
        size_t counts[levelCount] = {0, 0, 0, 0, 0, 0, 0};
        // copied on write: a dispatch holds the list it started with
        std::shared_ptr<const ListenerList> listeners = std::make_shared<ListenerList>();
        LogListenerId lastListenerId = 0;
        // a call of a removed listener ended (removeListener waits on it)
        std::mutex callsMutex;
        std::condition_variable callsDone;
        std::atomic<uint32_t> sinkId{0};
    };

    // never destroyed: threads and static destructors may still log while the process exits
    LogState& state(){
        static LogState* instance = new LogState();
        return *instance;
    }

    std::atomic<int> stackTraces{static_cast<int>(LogStackTraces::Warnings)};

    // the thread that loaded the engine until LuaBinding::createLuaState names the main one
    std::atomic<std::thread::id> mainThread{std::this_thread::get_id()};

    const std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();

    // a line logged inside a listener is recorded, not dispatched again
    bool& insideListener(){
        thread_local bool inside = false;
        return inside;
    }

    struct ListenerScope {
        bool previous;
        ListenerScope() : previous(insideListener()) { insideListener() = true; }
        ~ListenerScope() { insideListener() = previous; }
    };

    // the listener calls this thread is inside: a listener that removes itself does not wait
    // for its own call
    std::vector<const void*>& callsOnThisThread(){
        thread_local std::vector<const void*> calls;
        return calls;
    }

    // What the sink's write() leaves for its afterWrite() on this thread: the lines to hand to
    // the listeners (in id order) and a report of the worker lines the pending queue dropped
    struct ThreadDispatch {
        std::vector<LogEntry> entries;
        size_t droppedReport = 0;
    };

    ThreadDispatch& threadDispatch(){
        thread_local ThreadDispatch dispatch;
        return dispatch;
    }

    // Log::emitEntry's parts, for the record whose text it is (the text's address tells it from
    // a record logged meanwhile: a rate-limit summary, a drop notice)
    struct GivenParts {
        const char* text;
        const std::string* message;
        const std::string* stackTrace;
    };

    const GivenParts*& givenParts(){
        thread_local const GivenParts* parts = nullptr;
        return parts;
    }

    size_t levelIndex(LogLevel level){
        const size_t index = static_cast<size_t>(level);
        return index < levelCount ? index : static_cast<size_t>(LogLevel::Info);
    }

    std::string trimmed(std::string_view text){
        size_t start = 0;
        size_t end = text.size();
        while (start < end && (text[start] == ' ' || text[start] == '\t' || text[start] == '\n' || text[start] == '\r')) start++;
        while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\n' || text[end - 1] == '\r')) end--;
        return std::string(text.substr(start, end - start));
    }

    // Lua errors end with "stack traceback:" and the frames (LuaBinding::handleLuaError)
    void splitStackTrace(std::string_view text, std::string& message, std::string& stackTrace){
        const size_t at = text.find("stack traceback:");
        if (at == std::string_view::npos){
            message.assign(text.data(), text.size());
            stackTrace.clear();
            return;
        }
        message = trimmed(text.substr(0, at));
        stackTrace = trimmed(text.substr(at));
    }

    // One call, counted while it runs. The count goes up before `removed` is read, and
    // removeListener sets `removed` before it reads the count (both sequentially consistent):
    // either the call sees the removal and skips, or the removal sees the call and waits.
    struct CountedCall {
        Listener& listener;
        explicit CountedCall(Listener& listener) : listener(listener) {
            listener.calls.fetch_add(1);
            callsOnThisThread().push_back(&listener);
        }
        ~CountedCall() {
            callsOnThisThread().pop_back();
            // every call of a removed listener that ends wakes the removal (it waits for none left)
            listener.calls.fetch_sub(1);
            if (listener.removed.load()){
                LogState& s = state();
                std::lock_guard<std::mutex> lock(s.callsMutex);
                s.callsDone.notify_all();
            }
        }
    };

    void callListeners(const std::shared_ptr<const ListenerList>& listeners, const LogEntry& entry, LogDelivery delivery){
        for (const std::shared_ptr<Listener>& listener : *listeners){
            if (listener->delivery != delivery)
                continue;
            CountedCall call(*listener);
            if (!listener->removed.load())
                listener->function(entry);
        }
    }

    std::shared_ptr<const ListenerList> currentListeners(){
        LogState& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.listeners;
    }

    // the main thread's listeners and the Lua event
    void dispatchOnMainThread(const LogEntry& entry){
        const std::shared_ptr<const ListenerList> listeners = currentListeners();
        ListenerScope scope;
        callListeners(listeners, entry, LogDelivery::MainThread);
        if (Log::onMessageReceived.hasSubscribers())
            Log::onMessageReceived.call(entry.message, entry.stackTrace, static_cast<int>(entry.level));
    }

    // The buffer and the listeners as an S09 sink. Not the editor's own categories (editor,
    // build, mcp: Out::*, compiler output, the MCP server): the buffer is the game's log, as
    // upstream's (whose Log::* never saw Out::*); the sink's categoryMask leaves them out
    class LogBufferSink : public LogSink {
    public:
        LogBufferSink(){
            const uint64_t editorOnly = (1ull << logCategoryEditor().index) | (1ull << logCategoryBuild().index) |
                                        (1ull << logCategoryMcp().index);
            categoryMask.store(~editorOnly, std::memory_order_relaxed);
        }

        void write(const LogRecord& record) override {
            LogEntry entry;
            entry.id = record.id;
            entry.level = record.level;
            entry.category = record.category;
            const GivenParts* given = givenParts();
            const bool givenTrace = given && record.text.data() == given->text && !given->stackTrace->empty();
            if (givenTrace){
                entry.message = *given->message;
                entry.stackTrace = *given->stackTrace;
            }else{
                splitStackTrace(record.text, entry.message, entry.stackTrace);
            }
            entry.time = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
            entry.frame = Engine::getGameFrames();
            entry.mainThread = Log::isMainThread();
            // a Lua error's traceback came with its message (LuaBinding's "Lua Error: ..." lines)
            entry.fromLua = (record.flags & LogFlagLua) != 0 || (!givenTrace && !entry.stackTrace.empty());

            const bool dispatch = !insideListener();
            ThreadDispatch& out = threadDispatch();
            {
                LogState& s = state();
                std::lock_guard<std::mutex> lock(s.mutex);
                s.lastId = std::max(s.lastId, entry.id);
                s.counts[levelIndex(entry.level)]++;
                if (s.bufferSize > 0){
                    s.buffer.push_back(entry);
                    while (s.buffer.size() > s.bufferSize)
                        s.buffer.pop_front();
                }
                if (dispatch && !entry.mainThread){
                    if (s.pending.size() < pendingLimit){
                        if (s.pendingDropped > 0){
                            out.droppedReport += s.pendingDropped;
                            s.pendingDropped = 0;
                        }
                        s.pending.push_back(entry);
                    }else{
                        s.pendingDropped++;
                    }
                }
            }
            if (dispatch)
                out.entries.push_back(std::move(entry));
        }

        // the Logger's lock released: the listeners of what write() got on this thread
        void afterWrite() override {
            ThreadDispatch& out = threadDispatch();
            if (out.droppedReport > 0){
                const size_t dropped = out.droppedReport;
                out.droppedReport = 0;
                Log::warn("Log: %zu lines logged by other threads were not given to the log listeners (more than %zu waited for a frame)",
                          dropped, pendingLimit);
            }
            if (out.entries.empty())
                return;
            // taken first: a listener's own lines are recorded into the list of this thread
            std::vector<LogEntry> entries;
            entries.swap(out.entries);
            const std::shared_ptr<const ListenerList> listeners = currentListeners();
            for (const LogEntry& entry : entries){
                {
                    ListenerScope scope;
                    callListeners(listeners, entry, LogDelivery::AnyThread);
                }
                if (entry.mainThread)
                    dispatchOnMainThread(entry);
            }
            // the storage back for the next line (no allocation per line)
            entries.clear();
            if (out.entries.empty())
                out.entries.swap(entries);
        }

        Delivery delivery() const override { return Delivery::Inline; }
        const char* name() const override { return "buffer"; }
        bool resident() const override { return true; }
        bool wantsAfterWrite() const override { return true; }
    };

    // the sink in the Logger, once (the first use of the log, or the library's start)
    bool installBuffer(){
        static const bool installed = [](){
            state().sinkId.store(Logger::addSink(std::make_unique<LogBufferSink>()), std::memory_order_release);
            return true;
        }();
        return installed;
    }

    // at the engine library's start: records logged before any Log:: call are buffered too
    [[maybe_unused]] const bool bufferAtStart = installBuffer();

    void logAt(LogLevel level, const char* fmt, va_list args) JE_PRINTF_FORMAT(2, 0);
    void logAt(LogLevel level, const char* fmt, va_list args){
        installBuffer();
        LogCategory& category = logCategoryEngine();
        if (Logger::shouldLog(level, category))
            Logger::vwritef(level, category, nullptr, fmt, args);
    }
}

void Log::print(const char *fmt, ...) {
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    logAt(LogLevel::Info, fmt, arg_ptr);
    va_end(arg_ptr);
}

void Log::verbose(const char *fmt, ...) {
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    logAt(LogLevel::Trace, fmt, arg_ptr);
    va_end(arg_ptr);
}

void Log::debug(const char *fmt, ...) {
    #ifndef NDEBUG
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    logAt(LogLevel::Debug, fmt, arg_ptr);
    va_end(arg_ptr);
    #else
    (void)fmt;
    #endif
}

void Log::warn(const char *fmt, ...) {
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    logAt(LogLevel::Warn, fmt, arg_ptr);
    va_end(arg_ptr);
}

void Log::error(const char *fmt, ...) {
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    logAt(LogLevel::Error, fmt, arg_ptr);
    va_end(arg_ptr);
}

void Log::emitEntry(LogLevel level, const std::string& message, const std::string& stackTrace, bool fromLua,
                    const LogCategory* category, const LogSource* source){
    #ifdef NDEBUG
    if (level == LogLevel::Debug)
        return;
    #endif
    installBuffer();
    const LogCategory& target = category ? *category : (fromLua ? logCategoryLua() : logCategoryEngine());
    if (!Logger::shouldLog(level, target))
        return;
    // Lua lines carry their traceback in the text, never native frames
    const uint32_t flags = fromLua ? (LogFlagLua | LogFlagNoBacktrace) : 0u;
    if (stackTrace.empty()){
        Logger::write(level, target, message, source, flags);
        return;
    }
    // the platform log gets the trace under the message, as a Lua error's
    const std::string text = message + "\n" + stackTrace;
    const GivenParts parts{text.data(), &message, &stackTrace};
    const GivenParts* previous = givenParts();
    givenParts() = &parts;
    Logger::write(level, target, text, source, flags);
    givenParts() = previous;
}

uint32_t Log::getBufferSinkId(){
    installBuffer();
    return state().sinkId.load(std::memory_order_acquire);
}

LogListenerId Log::addListener(std::function<void(const LogEntry&)> listener, LogDelivery delivery){
    if (!listener)
        return 0;
    installBuffer();
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    auto added = std::make_shared<Listener>();
    added->id = ++s.lastListenerId;
    added->delivery = delivery;
    added->function = std::move(listener);
    auto list = std::make_shared<ListenerList>(*s.listeners);
    list->push_back(added);
    s.listeners = list;
    return added->id;
}

void Log::removeListener(LogListenerId id){
    LogState& s = state();
    std::shared_ptr<Listener> removed;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        auto list = std::make_shared<ListenerList>(*s.listeners);
        for (auto it = list->begin(); it != list->end(); ++it){
            if ((*it)->id == id){
                removed = *it;
                removed->removed.store(true);
                list->erase(it);
                break;
            }
        }
        s.listeners = list;
    }
    // From inside a listener call (any listener's, on this thread) no waiting: its own call,
    // or two listeners removing each other on two threads, would never end. No new call starts.
    if (!removed || !callsOnThisThread().empty())
        return;
    // the calls of it running on other threads end first
    std::unique_lock<std::mutex> lock(s.callsMutex);
    s.callsDone.wait(lock, [&removed](){ return removed->calls.load() == 0; });
}

void Log::dispatchPending(){
    // one line at a time: a listener may log (recorded, not dispatched) or clear the buffer
    while (true){
        LogEntry entry;
        {
            LogState& s = state();
            std::lock_guard<std::mutex> lock(s.mutex);
            if (s.pending.empty())
                return;
            entry = std::move(s.pending.front());
            s.pending.pop_front();
        }
        dispatchOnMainThread(entry);
    }
}

size_t Log::getPendingCount(){
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.pending.size();
}

std::vector<LogEntry> Log::getEntries(size_t maxCount, uint32_t levelMask, uint64_t afterId){
    std::vector<LogEntry> entries;
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    // newest first, then reversed: maxCount keeps the newest
    for (auto it = s.buffer.rbegin(); it != s.buffer.rend(); ++it){
        if (it->id <= afterId)
            break;
        if (!(levelMask & logLevelBit(it->level)))
            continue;
        entries.push_back(*it);
        if (maxCount > 0 && entries.size() >= maxCount)
            break;
    }
    std::reverse(entries.begin(), entries.end());
    return entries;
}

void Log::clearEntries(){
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.buffer.clear();
    for (size_t& count : s.counts)
        count = 0;
}

void Log::setBufferSize(size_t entries){
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.bufferSize = std::min(entries, maxBufferSize);
    while (s.buffer.size() > s.bufferSize)
        s.buffer.pop_front();
}

size_t Log::getBufferSize(){
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.bufferSize;
}

size_t Log::getCount(LogLevel level){
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.counts[levelIndex(level)];
}

uint64_t Log::getLastId(){
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.lastId;
}

uint64_t Log::getFirstBufferedId(){
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.buffer.empty() ? s.lastId + 1 : s.buffer.front().id;
}

void Log::setStackTraces(LogStackTraces mode){
    stackTraces.store(static_cast<int>(mode), std::memory_order_relaxed);
}

LogStackTraces Log::getStackTraces(){
    return static_cast<LogStackTraces>(stackTraces.load(std::memory_order_relaxed));
}

bool Log::wantsStackTrace(LogLevel level){
    switch (getStackTraces()){
        case LogStackTraces::All: return true;
        case LogStackTraces::Warnings: return level >= LogLevel::Warn && level <= LogLevel::Fatal;
        case LogStackTraces::Errors: return level == LogLevel::Error || level == LogLevel::Fatal;
        default: return false;
    }
}

void Log::setMainThread(){
    mainThread.store(std::this_thread::get_id(), std::memory_order_relaxed);
}

bool Log::isMainThread(){
    return std::this_thread::get_id() == mainThread.load(std::memory_order_relaxed);
}
