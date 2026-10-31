// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef log_h
#define log_h

#include "Export.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine {

    // FunctionSubscribe.h includes this header: the event type is declared here, defined there
    template<typename T> class FunctionSubscribe;

    // The level of a log line; the values are those of D_LOG_* (System.h), print being 0
    enum class LogLevel : uint8_t { Info = 0, Verbose = 1, Debug = 2, Warn = 3, Error = 4 };

    // the bit of a level in a level mask (Log::getEntries)
    constexpr uint32_t logLevelBit(LogLevel level) { return 1u << static_cast<uint32_t>(level); }
    constexpr uint32_t LOG_LEVELS_ALL = 0x1Fu;

    // One line of the log, as the buffer and the listeners get it
    struct LogEntry {
        uint64_t id = 0;            // increasing from 1, never reused
        LogLevel level = LogLevel::Info;
        std::string message;        // the text before "stack traceback:" (trimmed)
        std::string stackTrace;     // from "stack traceback:" on, a Lua traceback of Log.warn/error, or ""
        double time = 0.0;          // seconds since the app started (monotonic)
        uint64_t frame = 0;         // Engine::getGameFrames() when it was logged
        bool mainThread = true;     // logged on the main (Lua) thread
        bool fromLua = false;       // logged by Lua: print, Log.*, a Lua error
    };

    // MainThread listeners run on the main thread: at once for a line logged there, at the start
    // of the next frame (Log::dispatchPending) for a line logged by another thread. AnyThread
    // listeners run on the thread that logs, at once: they must be thread-safe.
    enum class LogDelivery { MainThread, AnyThread };
    using LogListenerId = uint64_t;

    // Which lines logged by Lua's Log.warn / Log.error carry a Lua traceback (Project Settings >
    // Debug > Log Stack Traces). Lua errors always carry theirs.
    enum class LogStackTraces { None, Errors, Warnings, All };

    // The engine's log. Every line goes to the platform log (the editor's Console, logcat, the
    // terminal), into a bounded ring buffer (the on-device console, Log.getEntries) and to the
    // listeners (Unity's Application.logMessageReceived / logMessageReceivedThreaded). Safe to
    // call from any thread; listeners are never called with the log's lock held, and a line
    // logged inside a listener is recorded but not handed to listeners again (no recursion).
    class JASPERENGINE_API Log {
    public:
        static void print(const char* text, ...);

        static void verbose(const char* text, ...);
        static void debug(const char* text, ...);   // compiled out with NDEBUG
        static void warn(const char* text, ...);
        static void error(const char* text, ...);

        // One line with its parts given (the Lua bindings): an empty stackTrace splits one off
        // the message at "stack traceback:"
        static void emitEntry(LogLevel level, const std::string& message, const std::string& stackTrace, bool fromLua);

        static LogListenerId addListener(std::function<void(const LogEntry&)> listener, LogDelivery delivery = LogDelivery::MainThread);
        // stops at once (a line being dispatched does not reach it any more)
        static void removeListener(LogListenerId id);
        // the lines other threads logged since the last call, to the MainThread listeners and
        // onMessageReceived, in order (Engine::runFrame, first thing of a frame)
        static void dispatchPending();
        // lines logged off the main thread that wait for dispatchPending
        static size_t getPendingCount();

        // The buffered lines, oldest first: the newest maxCount (0 = all) of the levels in
        // levelMask (logLevelBit) whose id is above afterId
        static std::vector<LogEntry> getEntries(size_t maxCount = 0, uint32_t levelMask = LOG_LEVELS_ALL, uint64_t afterId = 0);
        // empties the buffer and the counts
        static void clearEntries();
        // lines the buffer keeps (default 500; 0 = none, the oldest go first)
        static void setBufferSize(size_t entries);
        static size_t getBufferSize();
        // lines of that level logged since the last clearEntries (buffered or not)
        static size_t getCount(LogLevel level);
        // the id of the newest line logged (0 = none yet)
        static uint64_t getLastId();
        // the id of the oldest buffered line (getLastId() + 1 when the buffer is empty)
        static uint64_t getFirstBufferedId();

        static void setStackTraces(LogStackTraces mode);
        static LogStackTraces getStackTraces();
        // a line of this level logged by Lua's Log.* gets a traceback (the setting above)
        static bool wantsStackTrace(LogLevel level);

        // The thread that runs the frames and owns the Lua state (LuaBinding::createLuaState
        // sets it; until then the thread that loaded the engine)
        static void setMainThread();
        static bool isMainThread();

        // Lua's Log.onMessageReceived (Unity's Application.logMessageReceived): (message,
        // stackTrace, level) on the main thread. Scripts subscribe with RegisterEvent; a released
        // script's subscriptions go (Engine::removeSubscriptionsByTag)
        static FunctionSubscribe<void(const std::string&, const std::string&, int)> onMessageReceived;
    };

}

#endif
