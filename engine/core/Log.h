// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef log_h
#define log_h

#include "Export.h"
#include "log/LogCategory.h"
#include "log/LogFormatCheck.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine {

    // FunctionSubscribe.h includes this header: the event type is declared here, defined there
    template<typename T> class FunctionSubscribe;

    // SYNC4-ABSORB(log-overlay): upstream's log buffer and listeners on oe-port's one LogLevel
    // (log/LogCategory.h: Trace for upstream's Verbose); fed by Log::* only until the
    // log-overlay absorption makes them an S09 sink (every Logger record)
    // the bit of a level in a level mask (Log::getEntries)
    constexpr uint32_t logLevelBit(LogLevel level) { return 1u << static_cast<uint32_t>(level); }
    constexpr uint32_t LOG_LEVELS_ALL = 0x7Fu;   // Trace .. Off

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
    // (no None / Always enumerators here: X11 headers #define them, and exports include both)
    enum class LogStackTraces { Off, Errors, Warnings, All };

    // The engine's log. Every line goes to the platform log (the editor's Console, logcat, the
    // terminal), into a bounded ring buffer (the on-device console, Log.getEntries) and to the
    // listeners (Unity's Application.logMessageReceived / logMessageReceivedThreaded). Safe to
    // call from any thread; listeners are never called with the log's lock held, and a line
    // logged inside a listener is recorded but not handed to listeners again (no recursion).
    class JASPERENGINE_API Log {
    public:
        // printf-style, category "engine" (Logger in log/Logger.h has categories, source
        // locations and the JE_LOG_* macros): print is Info, verbose Trace, debug Debug
        // (compiled out with NDEBUG), warn Warn, error Error
        static void print(JE_FORMAT_STRING const char* text, ...) JE_PRINTF_FORMAT(1, 2);

        static void verbose(JE_FORMAT_STRING const char* text, ...) JE_PRINTF_FORMAT(1, 2);
        static void debug(JE_FORMAT_STRING const char* text, ...) JE_PRINTF_FORMAT(1, 2);
        static void warn(JE_FORMAT_STRING const char* text, ...) JE_PRINTF_FORMAT(1, 2);
        static void error(JE_FORMAT_STRING const char* text, ...) JE_PRINTF_FORMAT(1, 2);

        // One line with its parts given (the Lua bindings): an empty stackTrace splits one off
        // the message at "stack traceback:"
        static void emitEntry(LogLevel level, const std::string& message, const std::string& stackTrace, bool fromLua);

        static LogListenerId addListener(std::function<void(const LogEntry&)> listener, LogDelivery delivery = LogDelivery::MainThread);
        // Stops the listener. When this returns, no new call of it starts (a line being
        // dispatched does not reach it any more), and:
        //  - called outside any listener call: every call of it that was running (on any
        //    thread) has ended, so what the listener uses can go;
        //  - called from inside a listener call (that listener's or another's, on this thread):
        //    it does not wait, so calls running on other threads may still be finishing.
        // Waiting, it must not hold a lock the listener also takes (the listener would never end).
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
        // lines the buffer keeps (default 500; 0 = none, the oldest go first; at most maxBufferSize)
        static constexpr size_t maxBufferSize = 100000;
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
