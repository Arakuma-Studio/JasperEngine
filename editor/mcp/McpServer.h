// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "McpCall.h"
#include "McpJobs.h"
#include "McpJson.h"
#include "McpRegistry.h"

#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class App;
}

namespace jasperengine::editor::mcp {

    class McpContext;
    class McpTransportStdio;
    class McpTransportHttp;

    // How the process was launched (editor/main.cpp fills it before the backend starts).
    struct McpLaunchOptions {
        bool stdio = false;               // `jasperengine-editor mcp`: serve MCP on the original stdio
        int stdioOutFd = -1;              // dup of the original stdout (fd 1 is stderr by then)
        int stdioInFd = -1;               // dup of the original stdin (fd 0 is /dev/null by then)
        int httpPort = 0;                 // --http-port / --mcp-port; 0 = the Editor Settings choice
        std::filesystem::path project;    // --project: open this project (directory)
        std::filesystem::path createDir;  // --create: make a new project here
        std::string createName;           // --name for --create
        bool headless = false;
        bool toolGroupsSet = false;       // --tool-groups given (else settings mcp.tool_groups)
        std::vector<std::string> toolGroups;
    };

    McpLaunchOptions& mcpLaunchOptions();

    // The agent guide sent as initialize.instructions (McpInstructions.cpp)
    const char* mcpServerInstructions();

    // One MCP client connection: the stdio client, or one HTTP Mcp-Session-Id.
    struct McpSession {
        std::string id;
        std::string transport;            // "stdio" or "http"
        std::string clientName;
        std::string clientVersion;
        std::string protocolVersion;
        // Tool groups tools/list includes besides core (tool_groups changes them). Guarded by
        // the server's session mutex: McpServer::sessionToolGroups / setSessionToolGroups.
        std::set<std::string> toolGroups;
    };
    using SessionPtr = std::shared_ptr<McpSession>;

    // Where one request's answer goes. respond() is called exactly once; notify() carries
    // notifications related to the request (progress) before it. Any thread.
    class McpReply {
    public:
        virtual ~McpReply() = default;
        virtual void respond(const Json& message) = 0;
        virtual void notify(const Json& notification) = 0;
        // The client cancelled the request: MCP says no response is sent. Transports that
        // hold a connection open for the answer release it here.
        virtual void cancelled() {}
        // Whether notify() reaches the client: an HTTP request that does not accept
        // text/event-stream can only carry the response.
        virtual bool streamsNotifications() const { return true; }
    };
    using ReplyPtr = std::shared_ptr<McpReply>;

    // A tools/call as the Activity window lists it
    struct McpActivityEntry {
        uint64_t seq = 0;
        std::string client;
        std::string tool;
        std::string args;                 // shortened
        std::string status;               // running / ok / error / cancelled
        std::string message;              // shortened result text
        double durationMs = 0.0;
        std::chrono::system_clock::time_point time;
    };

    struct McpServerStatus {
        bool started = false;
        bool stdio = false;
        bool stdioOpen = false;
        bool httpRunning = false;
        int httpPort = 0;
        std::string httpError;
        std::vector<McpSession> sessions;  // copies
        size_t pendingCalls = 0;
        size_t runningJobs = 0;
    };

    // The MCP server embedded in the editor (docs/mcp/DESIGN.md). JSON-RPC 2.0 over the
    // stdio and Streamable HTTP transports; tools run on the editor main thread.
    class McpServer {
    public:
        static constexpr const char* SERVER_NAME = "jasperengine-editor";
        static constexpr int DEFAULT_HTTP_PORT = 47615;

        static McpServer& get();

        // Main thread, once the App exists: registers every tool module and starts the
        // transports the launch options / settings ask for.
        void start(App& app);
        // Main thread, before the backend tears down: stops HTTP, fails pending calls.
        void stop();
        // Main thread, every editor frame: runs deferred calls (waitUntil / afterFrames).
        void tick();

        bool isStarted() const { return started; }
        McpContext& context() { return *ctx; }
        McpRegistry& registry() { return reg; }
        McpJobs& jobs() { return jobList; }

        // HTTP transport control (Activity window / Editor Settings). Main thread.
        void setHttpEnabled(bool enabled, int port);
        McpServerStatus status() const;
        std::vector<McpActivityEntry> activity() const;
        void clearActivity();

        // --- transports ---
        SessionPtr createSession(const std::string& transport);
        SessionPtr findSession(const std::string& id) const;
        void closeSession(const std::string& id);
        // The tool groups a session lists besides core; any thread
        std::set<std::string> sessionToolGroups(const SessionPtr& session) const;
        void setSessionToolGroups(const SessionPtr& session, std::set<std::string> groups);
        // The tool groups new sessions start with (--tool-groups, else settings mcp.tool_groups)
        const std::set<std::string>& defaultToolGroups() const { return initialToolGroups; }
        // Handles one parsed JSON-RPC message (request, notification or response) from
        // any thread. Requests get exactly one reply->respond(); notifications and
        // responses get none.
        void handleMessage(const SessionPtr& session, const Json& message, const ReplyPtr& reply);
        // The stdio client went away (stdin EOF): the editor quits.
        void onStdioClosed();

        // --- calls (main thread) ---
        // Runs a tool inside another call (batch): same gates as a top-level call, but no
        // activity entry of its own. done runs when the nested call completes.
        void runNested(const CallPtr& parent, const std::string& tool, const Json& args,
                       std::function<void(const ToolResult&)> done);

        struct Deferred {
            CallPtr call;
            // waitUntil
            std::function<bool()> done;
            std::function<void(bool)> then;
            bool hasDeadline = false;
            std::chrono::steady_clock::time_point deadline;
            // afterFrames (frames >= 0)
            int frames = -1;
            std::function<void()> fn;
        };
        void addDeferred(Deferred deferred);
        bool hasDeferred(const McpCall* call) const;

        // A short description of the dialog blocking edits (empty if none)
        std::string blockingDialogMessage() const;

        // --- busy policy (main thread) ---
        // Why `activity` can't run now: a running job holding what it needs (JobBusy), or an
        // export started in the Build Settings window. Empty when it can run. forAgent adds
        // how to wait (get_job); `info` (optional) gets {job_id, kind, title, holds}.
        std::string busyRefusal(ToolActivity activity, Json* info = nullptr, bool forAgent = true) const;
        // Fails the call with busyRefusal's message when busy; true if it did.
        bool refuseIfBusy(const CallPtr& call, ToolActivity activity);
        // The activity the busy policy applies to a tool (resolves ToolActivity::Auto)
        static ToolActivity effectiveActivity(const ToolDef& def);

        static Json makeError(const Json& id, int code, const std::string& message, Json data = nullptr);
        static Json makeResult(const Json& id, Json result);

        // --- helpers for transports (any thread) ---
        // JASPER_MCP_TRACE=1: one stderr line per request read, run, answered and written
        static bool traceEnabled();
        static void trace(const char* stage, const std::string& detail);
        // Parses one JSON-RPC message. Invalid UTF-8 inside it is replaced by U+FFFD rather
        // than failing the whole request. Discarded (is_discarded()) when it is not JSON.
        static Json parseMessage(const std::string& text);
        // Replaces every invalid UTF-8 sequence with U+FFFD
        static std::string sanitizeUtf8(const std::string& text);
        // Cuts text to at most maxBytes without splitting a UTF-8 sequence
        static void truncateUtf8(std::string& text, size_t maxBytes);
        // Caps a tools/call result (text items and structuredContent) so one call can't
        // flood the client; truncated parts say so. Returns true if it changed anything.
        static bool capResultSize(Json& result);

    private:
        McpServer() = default;

        bool started = false;
        App* appPtr = nullptr;
        std::unique_ptr<McpContext> ctx;
        McpRegistry reg;
        McpJobs jobList;

        std::unique_ptr<McpTransportStdio> stdioTransport;
        std::unique_ptr<McpTransportHttp> httpTransport;

        mutable std::mutex sessionMutex;
        std::map<std::string, SessionPtr> sessions;
        uint64_t sessionCounter = 0;
        std::set<std::string> initialToolGroups;

        // In-flight requests by session id + JSON-RPC id, for notifications/cancelled
        mutable std::mutex inflightMutex;
        std::map<std::string, std::weak_ptr<McpCall>> inflight;

        std::vector<Deferred> deferred;   // main thread only

        mutable std::mutex activityMutex;
        std::deque<McpActivityEntry> activityLog;
        uint64_t activitySeq = 0;

        void chooseInitialToolGroups();
        Json listTools(const SessionPtr& session, const Json& params) const;
        void handleRequest(const SessionPtr& session, const Json& message, const ReplyPtr& reply);
        void handleToolsCall(const SessionPtr& session, const Json& id, const Json& params, const ReplyPtr& reply);
        void runOnMainThread(std::function<void()> task);
        // Gates + handler. complete() is already wired into the call.
        void execute(const CallPtr& call, const ToolDef& def);
        uint64_t beginActivity(const std::string& client, const std::string& tool, const Json& args);
        void endActivity(uint64_t seq, const ToolResult& result, double durationMs, bool cancelled);
        static std::string inflightKey(const std::string& sessionId, const Json& id);
    };

}
