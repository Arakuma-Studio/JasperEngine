// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Log.h"
#include "System.h"
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

    // Everything under one mutex; listeners are called from copies of the list, never under it
    struct LogState {
        std::mutex mutex;
        std::deque<LogEntry> buffer;
        size_t bufferSize = defaultBufferSize;
        std::deque<LogEntry> pending;
        size_t pendingDropped = 0;
        uint64_t lastId = 0;
        size_t counts[5] = {0, 0, 0, 0, 0};
        // copied on write: a dispatch holds the list it started with
        std::shared_ptr<const ListenerList> listeners = std::make_shared<ListenerList>();
        LogListenerId lastListenerId = 0;
        // a call of a removed listener ended (removeListener waits on it)
        std::mutex callsMutex;
        std::condition_variable callsDone;
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

    size_t levelIndex(LogLevel level){
        const size_t index = static_cast<size_t>(level);
        return index < 5 ? index : 0;
    }

    LogLevel levelOfType(int type){
        switch (type){
            case D_LOG_VERBOSE: return LogLevel::Verbose;
            case D_LOG_DEBUG: return LogLevel::Debug;
            case D_LOG_WARN: return LogLevel::Warn;
            case D_LOG_ERROR: return LogLevel::Error;
            default: return LogLevel::Info;
        }
    }

    int typeOfLevel(LogLevel level){
        switch (level){
            case LogLevel::Verbose: return D_LOG_VERBOSE;
            case LogLevel::Debug: return D_LOG_DEBUG;
            case LogLevel::Warn: return D_LOG_WARN;
            case LogLevel::Error: return D_LOG_ERROR;
            default: return 0;
        }
    }

    std::string trimmed(const std::string& text){
        size_t start = 0;
        size_t end = text.size();
        while (start < end && (text[start] == ' ' || text[start] == '\t' || text[start] == '\n' || text[start] == '\r')) start++;
        while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\n' || text[end - 1] == '\r')) end--;
        return text.substr(start, end - start);
    }

    // Lua errors end with "stack traceback:" and the frames (LuaBinding::handleLuaError)
    void splitStackTrace(const std::string& text, std::string& message, std::string& stackTrace){
        const size_t at = text.find("stack traceback:");
        if (at == std::string::npos){
            message = text;
            stackTrace.clear();
            return;
        }
        message = trimmed(text.substr(0, at));
        stackTrace = trimmed(text.substr(at));
    }

    // one formatted line: a stack buffer, the heap past 4 KB (a Lua traceback, a long print)
    std::string format(const char* fmt, va_list args){
        char stackBuffer[4096];
        va_list sizeArgs;
        va_copy(sizeArgs, args);
        const int size = vsnprintf(stackBuffer, sizeof(stackBuffer), fmt, sizeArgs);
        va_end(sizeArgs);
        if (size < 0)
            return std::string();
        if (size < static_cast<int>(sizeof(stackBuffer)))
            return std::string(stackBuffer, static_cast<size_t>(size));
        std::string text(static_cast<size_t>(size) + 1, '\0');
        va_list copy;
        va_copy(copy, args);
        vsnprintf(&text[0], text.size(), fmt, copy);
        va_end(copy);
        text.resize(static_cast<size_t>(size));
        return text;
    }

    void platformLogFormatted(int type, const char* fmt, ...){
        va_list args;
        va_start(args, fmt);
        System::instance().platformLog(type, fmt, args);
        va_end(args);
    }

    // Through the System's log; without one (before Engine::systemInit, or code that runs
    // without the engine: editor CLI tools) to stderr, never an abort
    void toPlatform(LogLevel level, const std::string& text){
        const int type = typeOfLevel(level);
        if (System::hasInstance()){
            platformLogFormatted(type, "%s", text.c_str());
            return;
        }
        const char* priority = type == D_LOG_VERBOSE ? "VERBOSE" : type == D_LOG_DEBUG ? "DEBUG" : type == D_LOG_WARN ? "WARN" : type == D_LOG_ERROR ? "ERROR" : nullptr;
        if (priority) fprintf(stderr, "(%s): ", priority);
        fprintf(stderr, "%s\n", text.c_str());
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
            if (listener.calls.fetch_sub(1) == 1 && listener.removed.load()){
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

    // the main thread's listeners and the Lua event
    void dispatchOnMainThread(const LogEntry& entry){
        std::shared_ptr<const ListenerList> listeners;
        {
            std::lock_guard<std::mutex> lock(state().mutex);
            listeners = state().listeners;
        }
        ListenerScope scope;
        callListeners(listeners, entry, LogDelivery::MainThread);
        if (Log::onMessageReceived.hasSubscribers())
            Log::onMessageReceived.call(entry.message, entry.stackTrace, static_cast<int>(entry.level));
    }

    void record(LogLevel level, const std::string& text, const std::string& givenTrace, bool fromLua){
        LogEntry entry;
        entry.level = level;
        if (givenTrace.empty()){
            splitStackTrace(text, entry.message, entry.stackTrace);
        }else{
            entry.message = text;
            entry.stackTrace = givenTrace;
        }
        entry.time = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
        entry.frame = Engine::getGameFrames();
        entry.mainThread = Log::isMainThread();
        // a Lua error's traceback came with its message (LuaBinding's "Lua Error: ..." lines)
        entry.fromLua = fromLua || (givenTrace.empty() && !entry.stackTrace.empty());

        const bool dispatch = !insideListener();
        std::shared_ptr<const ListenerList> listeners;
        size_t droppedReport = 0;
        {
            LogState& s = state();
            std::lock_guard<std::mutex> lock(s.mutex);
            entry.id = ++s.lastId;
            s.counts[levelIndex(level)]++;
            if (s.bufferSize > 0){
                s.buffer.push_back(entry);
                while (s.buffer.size() > s.bufferSize)
                    s.buffer.pop_front();
            }
            listeners = s.listeners;
            if (dispatch && !entry.mainThread){
                if (s.pending.size() < pendingLimit){
                    if (s.pendingDropped > 0){
                        droppedReport = s.pendingDropped;
                        s.pendingDropped = 0;
                    }
                    s.pending.push_back(entry);
                }else{
                    s.pendingDropped++;
                }
            }
        }

        // the platform log first: the editor's Console shows a line before what a listener logs
        // about it. A given trace goes under the message, as a Lua error's does
        toPlatform(level, givenTrace.empty() ? text : text + "\n" + givenTrace);

        if (droppedReport > 0)
            Log::warn("Log: %zu lines logged by other threads were not given to the log listeners (more than %zu waited for a frame)",
                      droppedReport, pendingLimit);

        if (!dispatch)
            return;
        {
            ListenerScope scope;
            callListeners(listeners, entry, LogDelivery::AnyThread);
        }
        if (entry.mainThread)
            dispatchOnMainThread(entry);
    }

    void emit(int type, const char* fmt, va_list args){
        record(levelOfType(type), format(fmt, args), std::string(), false);
    }
}

void Log::print(const char *fmt, ...) {
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    emit(0, fmt, arg_ptr);
    va_end(arg_ptr);
}

void Log::verbose(const char *fmt, ...) {
	va_list arg_ptr;
	va_start(arg_ptr, fmt);
    emit(D_LOG_VERBOSE, fmt, arg_ptr);
	va_end(arg_ptr);
}

void Log::debug(const char *fmt, ...) {
    #ifndef NDEBUG
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    emit(D_LOG_DEBUG, fmt, arg_ptr);
    va_end(arg_ptr);
    #endif
}

void Log::warn(const char *fmt, ...) {
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    emit(D_LOG_WARN, fmt, arg_ptr);
    va_end(arg_ptr);
}

void Log::error(const char *fmt, ...) {
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    emit(D_LOG_ERROR, fmt, arg_ptr);
    va_end(arg_ptr);
}

void Log::emitEntry(LogLevel level, const std::string& message, const std::string& stackTrace, bool fromLua){
    #ifdef NDEBUG
    if (level == LogLevel::Debug)
        return;
    #endif
    record(level, message, stackTrace, fromLua);
}

LogListenerId Log::addListener(std::function<void(const LogEntry&)> listener, LogDelivery delivery){
    if (!listener)
        return 0;
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
    if (!removed)
        return;
    // the calls of it running on other threads end first; this thread's own (a listener that
    // removes itself) cannot
    const std::vector<const void*>& here = callsOnThisThread();
    const int own = static_cast<int>(std::count(here.begin(), here.end(), static_cast<const void*>(removed.get())));
    std::unique_lock<std::mutex> lock(s.callsMutex);
    s.callsDone.wait(lock, [&removed, own](){ return removed->calls.load() <= own; });
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
        case LogStackTraces::Warnings: return level == LogLevel::Warn || level == LogLevel::Error;
        case LogStackTraces::Errors: return level == LogLevel::Error;
        default: return false;
    }
}

void Log::setMainThread(){
    mainThread.store(std::this_thread::get_id(), std::memory_order_relaxed);
}

bool Log::isMainThread(){
    return std::this_thread::get_id() == mainThread.load(std::memory_order_relaxed);
}
