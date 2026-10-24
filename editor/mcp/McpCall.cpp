// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpCall.h"

#include "McpServer.h"

using namespace jasperengine::editor::mcp;

ToolResult ToolResult::success(std::string text, Json structured) {
    ToolResult result;
    result.text = std::move(text);
    result.structured = std::move(structured);
    return result;
}

ToolResult ToolResult::error(std::string text, Json structured) {
    ToolResult result;
    result.text = std::move(text);
    result.structured = std::move(structured);
    result.isError = true;
    return result;
}

Json ToolResult::toJson() const {
    Json content = Json::array();
    if (!text.empty() || (images.empty() && extraContent.empty())) {
        content.push_back({{"type", "text"}, {"text", text}});
    }
    for (const ToolImage& image : images) {
        content.push_back({{"type", "image"}, {"data", image.data}, {"mimeType", image.mimeType}});
    }
    if (extraContent.is_array()) {
        for (const Json& item : extraContent) content.push_back(item);
    }
    Json out = {{"content", content}, {"isError", isError}};
    if (structured.is_object()) {
        out["structuredContent"] = structured;
    } else if (!structured.is_null()) {
        // structuredContent must be an object
        out["structuredContent"] = Json{{"value", structured}};
    }
    return out;
}

McpCall::McpCall(McpServer& server, std::string tool, Json args, Completion completion)
    : owner(server)
    , toolName(std::move(tool))
    , arguments(args.is_object() ? std::move(args) : Json::object())
    , completion(std::move(completion))
    , startTime(std::chrono::steady_clock::now()) {
    static std::atomic<uint64_t> nextSerial{1};
    callSerial = nextSerial++;
}

bool McpCall::cancelled() const {
    if (isCancelled.load()) return true;
    const std::shared_ptr<McpCall> parent = parentCall.lock();
    return parent && parent->cancelled();
}

McpContext& McpCall::ctx() {
    return owner.context();
}

void McpCall::ok(const std::string& text, Json structured) {
    reply(ToolResult::success(text, std::move(structured)));
}

void McpCall::fail(const std::string& message, Json structured) {
    reply(ToolResult::error(message, std::move(structured)));
}

void McpCall::reply(ToolResult result) {
    if (isCompleted.exchange(true)) return;
    if (finalizer) {
        try {
            finalizer(result);
        } catch (...) {
        }
        finalizer = nullptr;
    }
    if (completion) {
        Completion done = std::move(completion);
        completion = nullptr;
        done(*this, result);
    }
}

void McpCall::waitUntil(std::function<bool()> done, int timeoutMs, std::function<void(bool timedOut)> then) {
    McpServer::Deferred deferred;
    deferred.call = shared_from_this();
    deferred.done = std::move(done);
    deferred.then = std::move(then);
    if (timeoutMs > 0) {
        deferred.hasDeadline = true;
        deferred.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    }
    owner.addDeferred(std::move(deferred));
}

void McpCall::afterFrames(int frames, std::function<void()> fn) {
    McpServer::Deferred deferred;
    deferred.call = shared_from_this();
    deferred.frames = frames < 0 ? 0 : frames;
    deferred.fn = std::move(fn);
    owner.addDeferred(std::move(deferred));
}

void McpCall::progress(double done, double total, const std::string& message) {
    std::lock_guard<std::mutex> lock(progressMutex);
    if (progressSink && !isCompleted.load()) progressSink(done, total, message);
}

void McpCall::setProgressSink(ProgressSink sink) {
    std::lock_guard<std::mutex> lock(progressMutex);
    progressSink = std::move(sink);
}

double McpCall::elapsedMs() const {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startTime).count();
}
