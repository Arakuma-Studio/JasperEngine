// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef log_h
#define log_h

#include "Export.h"
#include "log/LogCategory.h"
#include "log/LogFormatCheck.h"
#include "log/LogRecord.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine {

    // FunctionSubscribe.h includes this header: the event type is declared here, defined there
    template<typename T> class FunctionSubscribe;

    // The log's buffer and listeners (feature debug-overlay) are an S09 sink: the resident
    // Inline sink "buffer" (Logger, log/LogSink.h) sees every record of the process, the
    // JE_LOG_* macros, Out::*, Lua and these Log::* calls alike, with the Logger's one LogLevel
    // (log/LogCategory.h; upstream's Verbose is Trace) and its ids. What the gate, the rate
    // limit or a full queue holds back never reaches it, as for every sink.
    // the bit of a level in a level mask (Log::getEntries)
    constexpr uint32_t logLevelBit(LogLevel level) { return 1u << static_cast<uint32_t>(level); }
    constexpr uint32_t LOG_LEVELS_ALL = 0x7Fu;   // Trace .. Off

    // One line of the log, as the buffer and the listeners get it
    struct LogEntry {
        uint64_t id = 0;            // the Logger's record id: increasing from 1, never reused
        LogLevel level = LogLevel::Info;
        const LogCategory* category = nullptr;   // the record's (engine for Log::*, lua for Lua)
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

    // The engine's log, on the S09 Logger (log/Logger.h). Every record goes to the Logger's
    // sinks (the editor's Console, logcat, the terminal, files) and, through the "buffer" sink,
    // into a bounded ring buffer (the on-device console, Log.getEntries) and to the listeners
    // (Unity's Application.logMessageReceived / logMessageReceivedThreaded). Safe to call from
    // any thread; listeners are never called with the Logger's or the buffer's lock held, and a
    // line logged inside a listener is recorded but not handed to listeners again (no recursion).
    class JASPERENGINE_API Log {
    public:
        // printf-style, category "engine" (Logger in log/Logger.h has categories, source
        // locations and the JE_LOG_* macros): print is Info, verbose Trace, debug Debug
        // (compiled out with NDEBUG), warn Warn, error Error. Gated before formatting and
        // rate limited by call site (S09 R3, R9).
        static void print(JE_FORMAT_STRING const char* text, ...) JE_PRINTF_FORMAT(1, 2);

        static void verbose(JE_FORMAT_STRING const char* text, ...) JE_PRINTF_FORMAT(1, 2);
        static void debug(JE_FORMAT_STRING const char* text, ...) JE_PRINTF_FORMAT(1, 2);
        static void warn(JE_FORMAT_STRING const char* text, ...) JE_PRINTF_FORMAT(1, 2);
        static void error(JE_FORMAT_STRING const char* text, ...) JE_PRINTF_FORMAT(1, 2);

        // One line with its parts given (the Lua bindings): logged as message, a new line and
        // stackTrace; the buffer gets the two parts as given (an empty stackTrace splits one off
        // the message at "stack traceback:"). Category: the one given, else lua for a line from
        // Lua (flagged LogFlagLua, no native backtrace) and engine otherwise.
        static void emitEntry(LogLevel level, const std::string& message, const std::string& stackTrace, bool fromLua,
                              const LogCategory* category = nullptr, const LogSource* source = nullptr);

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

        // The buffer's sink is in the Logger (it installs itself on first use of the log and at
        // the engine library's start); its Logger::SinkId, 0 if it was removed
        static uint32_t getBufferSinkId();

        // The buffered lines, oldest first: the newest maxCount (0 = all) of the levels in
        // levelMask (logLevelBit) whose id is above afterId
        static std::vector<LogEntry> getEntries(size_t maxCount = 0, uint32_t levelMask = LOG_LEVELS_ALL, uint64_t afterId = 0);
        // empties the buffer and the counts
        static void clearEntries();
        // lines the buffer keeps (default 500; 0 = none, the oldest go first; at most maxBufferSize)
        static constexpr size_t maxBufferSize = 100000;
        static void setBufferSize(size_t entries);
        static size_t getBufferSize();
        // lines of that level the buffer's sink got since the last clearEntries (buffered or not)
        static size_t getCount(LogLevel level);
        // the id of the newest line the buffer's sink got (0 = none yet)
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
