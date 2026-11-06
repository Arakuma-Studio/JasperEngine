// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "McpJson.h"
#include "command/UndoOrigin.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace jasperengine::editor::mcp {

    class McpContext;
    class McpServer;
    struct McpSession;
    using SessionPtr = std::shared_ptr<McpSession>;

    struct ToolImage {
        std::string data;                   // base64
        std::string mimeType = "image/png";
    };

    // A tools/call result: content [{type:text}, {type:image}...], structuredContent, isError.
    struct ToolResult {
        std::string text;
        std::vector<ToolImage> images;
        Json structured;                    // null = no structuredContent; otherwise an object
        bool isError = false;
        Json extraContent = Json::array();  // extra MCP content items, appended verbatim

        static ToolResult success(std::string text, Json structured = nullptr);
        static ToolResult error(std::string text, Json structured = nullptr);

        Json toJson() const;
    };

    // One tools/call, handed to the tool's handler on the main thread. A call completes
    // exactly once (ok / fail / reply); later completions are ignored. Exceptions thrown by
    // the handler, or by the waitUntil / afterFrames callbacks, become fail().
    // A handler must complete the call, or defer it (waitUntil / afterFrames), or detach()
    // it and complete it later; returning with none of these fails the call.
    class McpCall : public std::enable_shared_from_this<McpCall> {
    public:
        using Completion = std::function<void(McpCall&, const ToolResult&)>;
        using ProgressSink = std::function<void(double done, double total, const std::string& message)>;

        McpCall(McpServer& server, std::string tool, Json args, Completion completion);

        const std::string& tool() const { return toolName; }
        const Json& args() const { return arguments; }
        McpContext& ctx();
        McpServer& server() { return owner; }

        // Completion. Thread-safe; only the first one counts.
        void ok(const std::string& text, Json structured = nullptr);      // success
        void fail(const std::string& message, Json structured = nullptr);  // isError: true
        void reply(ToolResult result);                                     // text + images + structuredContent
        bool completed() const { return isCompleted.load(); }

        // Deferral: polls done() each frame on the main thread until it returns true or
        // timeoutMs passes (<= 0: no timeout), then calls then(timedOut). The handler
        // returns immediately; the editor keeps drawing frames while a call waits.
        void waitUntil(std::function<bool()> done, int timeoutMs, std::function<void(bool timedOut)> then);
        // Runs fn on the main thread after `frames` more editor frames (e.g. screenshots).
        void afterFrames(int frames, std::function<void()> fn);
        // The handler completes the call later from elsewhere (a job, a worker posting back).
        void detach() { isDetached = true; }
        bool detached() const { return isDetached; }
        // The main thread has begun running it (a queued call has not)
        void markStarted() { hasStarted = true; }
        bool started() const { return hasStarted.load(); }
        // Instead of its frames, if a file picker opens and holds the main thread meanwhile
        void setOnFilePicker(std::function<void()> fn) { onFilePicker = std::move(fn); }
        std::function<void()> takeOnFilePicker() { auto fn = std::move(onFilePicker); onFilePicker = nullptr; return fn; }

        // notifications/progress to the client, when it sent a progressToken. Best effort,
        // thread-safe; `total` <= 0 means unknown.
        void progress(double done, double total, const std::string& message);
        // The client sent notifications/cancelled for this request (or the editor is closing),
        // or for the batch this call runs in.
        bool cancelled() const;

        // Milliseconds since the call was created
        double elapsedMs() const;

        // The client session the call came from (a call inside a batch: the batch's); null
        // for calls the editor makes itself
        SessionPtr session() const;
        // A notification (tools/list_changed...) to the client, in this call's reply stream:
        // stdio sends it at once, HTTP inside the SSE response when the client accepts
        // text/event-stream. False when it can't reach the client. Thread-safe.
        bool notifyClient(const Json& notification);

        // Server side
        void setProgressSink(ProgressSink sink);
        // Adjusts the result right before completion (runs on the completing thread)
        void setFinalizer(std::function<void(ToolResult&)> fn) { finalizer = std::move(fn); }
        void requestCancel() { isCancelled = true; }
        // A call run inside another (batch): cancelling the parent cancels it too, and its
        // session and client notifications are the parent's
        void setParent(const std::shared_ptr<McpCall>& parent) { parentCall = parent; }
        // From a client the MCP window's "Allow changes" switch applies to (HTTP): with
        // changes off, only read-only tools run. Nested calls inherit it.
        void setChangesGated(bool gated) { gatedChanges = gated; }
        bool changesGated() const { return gatedChanges; }
        void setSession(SessionPtr clientSession) { callSession = std::move(clientSession); }
        void setNotifier(std::function<bool(const Json&)> fn);

        // Unique per editor session (the undo steps' attribution)
        uint64_t serial() const { return callSerial; }
        // Who the call's undo steps are from (McpServer::execute sets it for edit tools; the
        // kind stays User for read-only and control tools, which are not attributed)
        void setUndoOrigin(UndoOrigin origin) { undoOrigin = std::move(origin); }
        const UndoOrigin& getUndoOrigin() const { return undoOrigin; }
        bool attributed() const { return undoOrigin.kind == UndoOriginKind::Agent; }
        // The client's name (MCP initialize clientInfo.name) and the request id
        void setClient(std::string name, std::string requestId) { clientName = std::move(name); callRequestId = std::move(requestId); }
        const std::string& client() const { return clientName; }
        const std::string& requestId() const { return callRequestId; }

    private:
        McpServer& owner;
        std::string toolName;
        Json arguments;
        Completion completion;
        ProgressSink progressSink;
        std::function<bool(const Json&)> notifier;
        SessionPtr callSession;
        std::function<void(ToolResult&)> finalizer;
        std::mutex progressMutex;
        std::atomic<bool> isCompleted{false};
        std::atomic<bool> isCancelled{false};
        std::atomic<bool> hasStarted{false};
        std::function<void()> onFilePicker;     // main thread only
        bool isDetached = false;
        bool gatedChanges = false;
        std::weak_ptr<McpCall> parentCall;
        std::chrono::steady_clock::time_point startTime;
        uint64_t callSerial = 0;
        UndoOrigin undoOrigin;
        std::string clientName;
        std::string callRequestId;
    };

    using CallPtr = std::shared_ptr<McpCall>;

}
