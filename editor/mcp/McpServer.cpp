// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpServer.h"
#include "log/LogSinks.h"

#include "McpAutoSync.h"
#include "McpContext.h"
#include "McpDialogs.h"
#include "McpInstances.h"
#include "McpToolGroups.h"
#include "McpTransportHttp.h"
#include "McpTransportStdio.h"
#include "McpUndo.h"
#include "tools/McpTools.h"
#include "tools/W4Shared.h"

#include "App.h"
#include "AppSettings.h"
#include "EditorVersion.h"
#include "Out.h"
#include "Platform.h"
#include "window/EditorFilePicker.h"
#include "thread/ResourceProgress.h"
#include "Project.h"
#include "util/ProjectFileWrites.h"
#include "profiler/Profile.h"
#include "profiler/FrameTimes.h"
#include "util/FileUtils.h"
#include "util/StringUtils.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <optional>

using namespace jasperengine::editor;
using namespace jasperengine::editor::mcp;

namespace {

// A tool's handler is running on this stack: McpServer::runSettles waits for it to return
struct HandlerOnStack {
    int& depth;
    explicit HandlerOnStack(int& depthIn) : depth(depthIn) { depth++; }
    ~HandlerOnStack() { depth--; }
};

// Versions that open with an initialize handshake, newest first: an unknown client version
// gets the newest. 2026-07-28 (McpServer::MODERN_PROTOCOL_VERSION) has no handshake.
const char* const LEGACY_PROTOCOL_VERSIONS[] = {
    "2025-11-25",
    "2025-06-18",
    "2025-03-26",
    "2024-11-05"
};

constexpr const char* META_PROTOCOL_VERSION = "io.modelcontextprotocol/protocolVersion";
constexpr const char* META_CLIENT_INFO = "io.modelcontextprotocol/clientInfo";
constexpr const char* META_CLIENT_CAPABILITIES = "io.modelcontextprotocol/clientCapabilities";
constexpr const char* META_SERVER_INFO = "io.modelcontextprotocol/serverInfo";

// 2026-07-28 error code
constexpr int UNSUPPORTED_PROTOCOL_VERSION = -32022;

// Results 2026-07-28 gives cache hints
bool cacheableMethod(const std::string& method) {
    return method == "server/discover" || method == "tools/list" || method == "prompts/list" ||
           method == "resources/list" || method == "resources/templates/list" || method == "resources/read";
}

// A 2026-07-28 request's answer: every result says resultType "complete" and who answered
// it; the cacheable ones are always stale (turning changes on or off for HTTP changes the
// tool list, and resources read the open project) and private to the token
class ModernReply : public McpReply {
public:
    ModernReply(ReplyPtr inner, std::string method)
        : inner(std::move(inner)), method(std::move(method)) {
    }

    void respond(const Json& message) override {
        if (!message.contains("result") || !message["result"].is_object()) {
            inner->respond(message);
            return;
        }
        Json out = message;
        Json& result = out["result"];
        result["resultType"] = "complete";
        if (cacheableMethod(method)) {
            result["ttlMs"] = 0;
            result["cacheScope"] = "private";
        }
        if (!result.contains("_meta") || !result["_meta"].is_object()) result["_meta"] = Json::object();
        result["_meta"][META_SERVER_INFO] = McpServer::serverInfo();
        inner->respond(out);
    }

    void respondError(const Json& message, int httpStatus) override {
        inner->respondError(message, httpStatus);
    }

    void notify(const Json& notification) override {
        inner->notify(notification);
    }

    void cancelled() override {
        inner->cancelled();
    }

private:
    ReplyPtr inner;
    std::string method;
};

constexpr size_t ACTIVITY_LOG_SIZE = 300;

std::string shorten(const std::string& text, size_t max) {
    std::string out;
    out.reserve(std::min(text.size(), max));
    for (char c : text) {
        if (out.size() >= max) break;
        out.push_back(c == '\n' || c == '\r' || c == '\t' ? ' ' : c);
    }
    if (text.size() > max) out += "...";
    return out;
}

} // namespace

McpLaunchOptions& jasperengine::editor::mcp::mcpLaunchOptions() {
    static McpLaunchOptions options;
    return options;
}

McpServer& McpServer::get() {
    // Never destroyed: transport threads may still be blocked on a read at exit
    static McpServer* server = new McpServer();
    return *server;
}

void McpServer::start(App& app) {
    if (started) return;
    started = true;
    // the folders an MCP call's strict write scope lets writes reach outside the project
    safewrite::allowOutsideScopes(AppSettings::getConfigDirectory());
    {
        std::error_code tempError;
        safewrite::allowOutsideScopes(std::filesystem::temp_directory_path(tempError));
    }
    appPtr = &app;
    ctx = std::make_unique<McpContext>(app, *this);
    McpAutoSync::get().serverStarted();
    installPoseSyncHook(*ctx);
    // the editor's own file writes (its windows' Save, the sprite normals watcher) go through the
    // file sync while the server runs: their files are current for the automatic pick-up, not
    // external changes
    ProjectFileWrites::setSyncHook([](Project* project, const std::vector<std::filesystem::path>& changed) {
        McpServer& server = McpServer::get();
        if (!server.isStarted() || server.context().project() != project) return false;
        w4::syncFiles(server.context(), changed, false);
        return true;
    });

    buildToolRegistry(reg);
    chooseInitialToolGroups();

    EditorFilePicker::Host pickerHost;
    pickerHost.route = [this]() {
        if (!Platform::isHeadless()) return EditorFilePicker::Route::Native;
        std::lock_guard<std::mutex> lock(sessionMutex);
        return sessions.empty() ? EditorFilePicker::Route::Cancel : EditorFilePicker::Route::Agent;
    };
    pickerHost.opened = [this]() { onFilePickerOpened(); };
    pickerHost.baseDirectory = [this]() {
        Project* project = ctx ? ctx->project() : nullptr;
        return project ? FileUtils::pathToUtf8(project->getProjectPath()) : std::string();
    };
    EditorFilePicker::setHost(std::move(pickerHost));

    // notifications/message: off until a session asks (logging/setLevel)
    auto sink = std::make_unique<CallbackSink>("mcp", [this](const LogRecord& record) { sendLogNotification(record); });
    sink->minLevel.store(LogLevel::Off, std::memory_order_relaxed);
    logSink = Logger::addSink(std::move(sink));
    httpChangesAllowed = AppSettings::getMcpHttpAllowChanges();

    const McpLaunchOptions& options = mcpLaunchOptions();
#ifndef _WIN32
    // A client that disconnects mid-write must not kill the editor
    std::signal(SIGPIPE, SIG_IGN);
#endif

    if (options.stdio && options.stdioOutFd >= 0 && options.stdioInFd >= 0) {
        stdioTransport = std::make_unique<McpTransportStdio>(*this, options.stdioInFd, options.stdioOutFd);
        stdioTransport->start();
    }

    int port = options.httpPort;
    if (port <= 0 && AppSettings::getMcpHttpEnabled()) {
        port = AppSettings::getMcpHttpPort();
    }
    if (port > 0) {
        setHttpEnabled(true, port);
    }
}

void McpServer::chooseInitialToolGroups() {
    const McpLaunchOptions& options = mcpLaunchOptions();
    std::vector<std::string> groups = options.toolGroups;
    if (!options.toolGroupsSet) {
        // `mcp --tool-groups` validated its names before the editor started; a stale name in
        // the settings must not keep the editor from starting
        std::string error;
        if (!parseToolGroupList(AppSettings::getMcpToolGroups(), groups, error)) {
            Out::warning("Editor settings mcp.tool_groups: %s; no tool group is enabled by default.", error.c_str());
            groups.clear();
        }
    }
    initialToolGroups = std::set<std::string>(groups.begin(), groups.end());
}

Json McpServer::listTools(const SessionPtr& session, const Json& params) const {
    // params._meta["jasper/groups"]: "all" or a list of groups instead of the session's (the
    // CLI, the test harness and the docs/budget generators list every group this way)
    bool all = false;
    std::set<std::string> groups;
    const Json* requested = nullptr;
    if (params.contains("_meta") && params["_meta"].is_object() && params["_meta"].contains("jasper/groups")) {
        requested = &params["_meta"]["jasper/groups"];
    }
    if (requested && requested->is_string() && requested->get<std::string>() == "all") {
        all = true;
    } else if (requested && requested->is_array()) {
        for (const Json& item : *requested) {
            if (item.is_string()) groups.insert(item.get<std::string>());
        }
    } else {
        groups = sessionToolGroups(session);
    }
    Json tools = Json::array();
    for (const ToolDef& def : reg.tools()) {
        const std::string& group = McpRegistry::groupOf(def);
        // an HTTP client with changes off (the MCP window's "Allow changes") lists the read-only ones
        if (session && toolHidden(*session, def)) continue;
        if (all || group == CORE_TOOL_GROUP || groups.count(group)) tools.push_back(McpRegistry::toolToJson(def));
    }
    return Json{{"tools", tools}};
}

void McpServer::stop() {
    if (!started) return;
    EditorFilePicker::abandon();
    EditorFilePicker::setHost({});
    if (logSink) {
        Logger::removeSink(logSink);
        logSink = 0;
    }
    if (httpTransport) {
        httpTransport->stop();
    }
    instanceRecord.httpStopped();
    // Calls still waiting on frames would never finish now
    std::vector<Deferred> pending;
    pending.swap(deferred);
    for (Deferred& item : pending) {
        item.call->requestCancel();
        item.call->fail("The editor is shutting down.");
    }
    // Answers still waiting for their settle go out now; what completes after this answers at once
    {
        std::lock_guard<std::mutex> lock(settleMutex);
        settlesClosed = true;
    }
    runSettles();
    // Let the answers already given reach the stdio client before the process exits
    if (stdioTransport) stdioTransport->flush(2000);
}

void McpServer::setHttpEnabled(bool enabled, int port) {
    if (!httpTransport) {
        httpTransport = std::make_unique<McpTransportHttp>(*this);
    }
    if (!enabled) {
        httpTransport->stop();
        instanceRecord.httpStopped();
        return;
    }
    // The first time HTTP is on: from now on clients need the token
    if (AppSettings::getMcpHttpToken().empty()) {
        AppSettings::setMcpHttpToken(McpTransportHttp::newToken());
    }
    httpTransport->setToken(AppSettings::getMcpHttpToken());
    if (httpTransport->isRunning() && httpTransport->port() == port) return;
    std::string error;
    if (httpTransport->start(port, error)) {
        Out::info("MCP server listening on http://127.0.0.1:%d/mcp", port);
        // `jasperengine-editor call` finds this editor through its instance file
        instanceRecord.httpStarted(port, !AppSettings::getMcpHttpToken().empty(), currentProjectPath());
    } else {
        instanceRecord.httpStopped();
        Out::error("MCP HTTP server could not start on port %d: %s", port, error.c_str());
    }
}

std::string McpServer::httpToken() const {
    return AppSettings::getMcpHttpToken();
}

std::string McpServer::newHttpToken() {
    const std::string token = McpTransportHttp::newToken();
    AppSettings::setMcpHttpToken(token);
    if (httpTransport) httpTransport->setToken(token);
    Out::info("MCP: made a new HTTP token; HTTP clients need it to keep working");
    return token;
}

void McpServer::setHttpAllowChanges(bool allow) {
    httpChangesAllowed = allow;
    AppSettings::setMcpHttpAllowChanges(allow);
}

bool McpServer::toolHidden(const McpSession& session, const ToolDef& def) const {
    return session.transport == "http" && !httpChangesAllowed.load() && !def.annotations.readOnly;
}

std::string McpServer::instructionsFor(const McpSession& session) const {
    std::string text = mcpServerInstructions();
    if (session.transport == "http" && !httpChangesAllowed.load()) {
        text += "\n\nREAD-ONLY: the user turned off changes for MCP clients over HTTP (MCP window: \"Allow changes\"), "
                "so only the read-only tools are listed and run: inspect, take screenshots, read files and the Console, "
                "and suggest changes instead of making them.";
    }
    return text;
}

McpServerStatus McpServer::status() const {
    McpServerStatus out;
    out.started = started;
    out.httpAllowChanges = httpChangesAllowed.load();
    out.stdio = stdioTransport != nullptr;
    out.stdioOpen = stdioTransport && stdioTransport->isOpen();
    if (httpTransport) {
        out.httpRunning = httpTransport->isRunning();
        out.httpPort = httpTransport->port();
        out.httpError = httpTransport->lastError();
    }
    {
        std::lock_guard<std::mutex> lock(sessionMutex);
        for (const auto& item : sessions) {
            out.sessions.push_back(*item.second);
        }
    }
    out.pendingCalls = deferred.size();
    out.runningJobs = jobList.runningCount();
    return out;
}

std::vector<McpActivityEntry> McpServer::activity() const {
    std::lock_guard<std::mutex> lock(activityMutex);
    return std::vector<McpActivityEntry>(activityLog.begin(), activityLog.end());
}

void McpServer::clearActivity() {
    std::lock_guard<std::mutex> lock(activityMutex);
    activityLog.erase(std::remove_if(activityLog.begin(), activityLog.end(),
        [](const McpActivityEntry& entry) { return entry.status != "running"; }), activityLog.end());
}

uint64_t McpServer::beginActivity(const std::string& client, const std::string& tool, const Json& args) {
    std::lock_guard<std::mutex> lock(activityMutex);
    McpActivityEntry entry;
    entry.seq = ++activitySeq;
    entry.client = client;
    entry.tool = tool;
    entry.args = shorten(args.dump(), 160);
    entry.status = "running";
    entry.time = std::chrono::system_clock::now();
    activityLog.push_back(entry);
    while (activityLog.size() > ACTIVITY_LOG_SIZE) activityLog.pop_front();
    return entry.seq;
}

void McpServer::endActivity(uint64_t seq, const ToolResult& result, double durationMs, bool cancelled) {
    std::lock_guard<std::mutex> lock(activityMutex);
    for (auto it = activityLog.rbegin(); it != activityLog.rend(); ++it) {
        if (it->seq != seq) continue;
        it->status = cancelled ? "cancelled" : (result.isError ? "error" : "ok");
        it->message = shorten(result.text, 200);
        it->durationMs = durationMs;
        break;
    }
}

// ---------------------------------------------------------------- log notifications

const char* McpServer::mcpLogLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Trace:
        case LogLevel::Debug: return "debug";
        case LogLevel::Info:  return "info";
        case LogLevel::Warn:  return "warning";
        case LogLevel::Error: return "error";
        default:              return "critical";
    }
}

bool McpServer::parseMcpLogLevel(const std::string& name, LogLevel& out) {
    if (name == "debug") out = LogLevel::Debug;
    else if (name == "info" || name == "notice") out = LogLevel::Info;
    else if (name == "warning") out = LogLevel::Warn;
    else if (name == "error") out = LogLevel::Error;
    else if (name == "critical" || name == "alert" || name == "emergency") out = LogLevel::Fatal;
    else return false;
    return true;
}

void McpServer::setSessionLogLevel(const SessionPtr& session, LogLevel level) {
    {
        std::lock_guard<std::mutex> lock(sessionMutex);
        session->logLevel = level == LogLevel::Off ? -1 : static_cast<int>(level);
    }
    updateLogSinkLevel();
}

void McpServer::setAllSessionsLogLevel(LogLevel level) {
    {
        std::lock_guard<std::mutex> lock(sessionMutex);
        for (auto& [id, session] : sessions) {
            session->logLevel = level == LogLevel::Off ? -1 : static_cast<int>(level);
        }
    }
    updateLogSinkLevel();
}

void McpServer::updateLogSinkLevel() {
    if (!logSink) return;
    int lowest = static_cast<int>(LogLevel::Off);
    {
        std::lock_guard<std::mutex> lock(sessionMutex);
        for (const auto& [id, session] : sessions) {
            if (session->logLevel >= 0) lowest = std::min(lowest, session->logLevel);
        }
    }
    Logger::setSinkLevel(logSink, static_cast<LogLevel>(lowest));
}

void McpServer::sendLogNotification(const LogRecord& record) {
    // the logger's drain thread: copies only, the transports queue what they send
    std::vector<SessionPtr> listeners;
    {
        std::lock_guard<std::mutex> lock(sessionMutex);
        for (const auto& [id, session] : sessions) {
            if (session->logLevel >= 0 && static_cast<int>(record.level) >= session->logLevel) listeners.push_back(session);
        }
    }
    if (listeners.empty()) return;
    std::string text(record.text);
    truncateUtf8(text, 16 * 1024);
    Json data = sanitizeUtf8(text);
    const Json notification = {
        {"jsonrpc", "2.0"},
        {"method", "notifications/message"},
        {"params", {{"level", mcpLogLevelName(record.level)}, {"logger", record.category->name}, {"data", data}}}
    };
    for (const SessionPtr& session : listeners) {
        if (session->transport == "stdio") {
            // at any time on stdio
            if (stdioTransport) stdioTransport->send(notification);
            continue;
        }
        // on HTTP, on the SSE stream of a request in flight (there is no standalone stream)
        std::shared_ptr<McpReply> reply;
        {
            std::lock_guard<std::mutex> lock(inflightMutex);
            const std::string prefix = session->id + "|";
            for (auto it = inflightReplies.lower_bound(prefix); it != inflightReplies.end() &&
                 it->first.compare(0, prefix.size(), prefix) == 0; ++it) {
                if ((reply = it->second.lock())) break;
            }
        }
        if (reply) reply->notify(notification);
    }
}

// ---------------------------------------------------------------- sessions

SessionPtr McpServer::createSession(const std::string& transport) {
    auto session = std::make_shared<McpSession>();
    session->transport = transport;
    std::lock_guard<std::mutex> lock(sessionMutex);
    session->toolGroups = initialToolGroups;
    if (transport == "stdio") {
        session->id = "stdio";
    } else {
        // Not a secret (the server binds to localhost), only unique per run
        const uint64_t salt = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%llx-%llu",
                      static_cast<unsigned long long>(salt * 2654435761ull), static_cast<unsigned long long>(++sessionCounter));
        session->id = buffer;
    }
    sessions[session->id] = session;
    return session;
}

SessionPtr McpServer::createStatelessSession(const std::string& transport) {
    auto session = std::make_shared<McpSession>();
    session->transport = transport;
    session->stateless = true;
    session->protocolVersion = MODERN_PROTOCOL_VERSION;
    std::lock_guard<std::mutex> lock(sessionMutex);
    // Keys its in-flight request (cancellation) apart from every other request's
    session->id = "request-" + std::to_string(++sessionCounter);
    return session;
}

SessionPtr McpServer::findSession(const std::string& id) const {
    std::lock_guard<std::mutex> lock(sessionMutex);
    auto it = sessions.find(id);
    return it != sessions.end() ? it->second : nullptr;
}

std::set<std::string> McpServer::sessionToolGroups(const SessionPtr& session) const {
    if (!session) return initialToolGroups;
    std::lock_guard<std::mutex> lock(sessionMutex);
    return session->toolGroups;
}

void McpServer::setSessionToolGroups(const SessionPtr& session, std::set<std::string> groups) {
    if (!session) return;
    std::lock_guard<std::mutex> lock(sessionMutex);
    session->toolGroups = std::move(groups);
}

void McpServer::closeSession(const std::string& id) {
    {
        std::lock_guard<std::mutex> lock(sessionMutex);
        sessions.erase(id);
        // nobody left to answer a picker that waits for an agent
        if (sessions.empty()) EditorFilePicker::abandon();
    }
    updateLogSinkLevel();
    // Its pending requests have nobody to answer to
    std::lock_guard<std::mutex> lock(inflightMutex);
    const std::string prefix = id + "|";
    for (auto it = inflight.begin(); it != inflight.end();) {
        if (it->first.compare(0, prefix.size(), prefix) == 0) {
            if (CallPtr call = it->second.lock()) call->requestCancel();
            inflightReplies.erase(it->first);
            it = inflight.erase(it);
        } else {
            ++it;
        }
    }
}

void McpServer::onStdioClosed() {
    std::fprintf(stderr, "[mcp] stdin closed: the MCP client is gone, closing the editor.\n");
    closeSession("stdio");
    runOnMainThread([this]() {
        if (appPtr) appPtr->quitWithoutPrompts("MCP client disconnected");
    });
}

std::string McpServer::inflightKey(const std::string& sessionId, const Json& id) {
    return sessionId + "|" + id.dump();
}

void McpServer::cancelRequest(const std::string& sessionId, const Json& requestId) {
    std::lock_guard<std::mutex> lock(inflightMutex);
    auto it = inflight.find(inflightKey(sessionId, requestId));
    if (it != inflight.end()) {
        if (CallPtr call = it->second.lock()) call->requestCancel();
    }
}

// ---------------------------------------------------------------- JSON-RPC

Json McpServer::makeError(const Json& id, int code, const std::string& message, Json data) {
    Json error = {{"code", code}, {"message", message}};
    if (!data.is_null()) error["data"] = std::move(data);
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", error}};
}

Json McpServer::makeResult(const Json& id, Json result) {
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

Json McpServer::supportedProtocolVersions() {
    Json versions = Json::array({MODERN_PROTOCOL_VERSION});
    for (const char* version : LEGACY_PROTOCOL_VERSIONS) versions.push_back(version);
    return versions;
}

bool McpServer::isLegacyProtocolVersion(const std::string& version) {
    for (const char* legacy : LEGACY_PROTOCOL_VERSIONS) {
        if (version == legacy) return true;
    }
    return false;
}

std::string McpServer::requestProtocolVersion(const Json& message) {
    if (!message.is_object() || !message.contains("params") || !message["params"].is_object()) return {};
    const Json& params = message["params"];
    if (!params.contains("_meta") || !params["_meta"].is_object()) return {};
    const Json& meta = params["_meta"];
    if (!meta.contains(META_PROTOCOL_VERSION) || !meta[META_PROTOCOL_VERSION].is_string()) return {};
    return meta[META_PROTOCOL_VERSION].get<std::string>();
}

Json McpServer::serverInfo() {
    return Json{{"name", SERVER_NAME}, {"title", "JasperEngine Editor"}, {"version", JASPERENGINE_EDITOR_VERSION}};
}

// ---------------------------------------------------------------- transport helpers

namespace {

constexpr size_t MAX_RESULT_TEXT_BYTES = 256 * 1024;
constexpr size_t MAX_RESULT_STRUCTURED_BYTES = 1024 * 1024;

} // namespace

bool McpServer::traceEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("JASPER_MCP_TRACE");
        return value && *value && std::string(value) != "0";
    }();
    return enabled;
}

void McpServer::trace(const char* stage, const std::string& detail) {
    if (!traceEnabled()) return;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::fprintf(stderr, "[mcp-trace %lld] %s %s\n", static_cast<long long>(ms % 100000000), stage, detail.c_str());
}

std::string McpServer::sanitizeUtf8(const std::string& text) {
    // overlong forms, surrogates and code points past U+10FFFF are invalid, as nlohmann::json's
    // parser says
    return StringUtils::repairUtf8(text);
}

void McpServer::truncateUtf8(std::string& text, size_t maxBytes) {
    if (text.size() <= maxBytes) return;
    size_t cut = maxBytes;
    // back up over continuation bytes to the start of the sequence that straddles the cut
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) cut--;
    text.resize(cut);
}

Json McpServer::parseMessage(const std::string& text) {
    Json message = Json::parse(text, nullptr, false);
    if (!message.is_discarded()) return message;
    // Most often a client that sent Latin-1 or cut a string mid-character: keep the request
    const std::string repaired = sanitizeUtf8(text);
    if (repaired != text) message = Json::parse(repaired, nullptr, false);
    return message;
}

bool McpServer::capResultSize(Json& result) {
    bool changed = false;
    if (result.contains("content") && result["content"].is_array()) {
        for (Json& item : result["content"]) {
            if (!item.is_object() || item.value("type", "") != "text" || !item.contains("text") || !item["text"].is_string()) continue;
            std::string& text = item["text"].get_ref<std::string&>();
            if (text.size() <= MAX_RESULT_TEXT_BYTES) continue;
            const size_t original = text.size();
            truncateUtf8(text, MAX_RESULT_TEXT_BYTES);
            text += "\n[... truncated by the MCP server: " + std::to_string(text.size()) + " of " + std::to_string(original) +
                    " bytes shown. Ask for less (the tool's limit / detail / offset / path arguments).]";
            changed = true;
        }
    }
    if (result.contains("structuredContent")) {
        const size_t size = result["structuredContent"].dump(-1, ' ', false, Json::error_handler_t::replace).size();
        if (size > MAX_RESULT_STRUCTURED_BYTES) {
            result["structuredContent"] = Json{
                {"truncated", true},
                {"original_bytes", size},
                {"note", "structuredContent was too large to send (over " + std::to_string(MAX_RESULT_STRUCTURED_BYTES / 1024) +
                         " KB) and was dropped; the text content has the summary. Ask for less (limit / detail / offset arguments)."}
            };
            changed = true;
        }
    }
    return changed;
}

void McpServer::handleMessage(const SessionPtr& session, const Json& message, const ReplyPtr& reply) {
    if (!message.is_object()) {
        if (reply) reply->respond(makeError(nullptr, -32600, "Invalid Request: expected a JSON-RPC object (batches are not supported)."));
        return;
    }
    const bool hasMethod = message.contains("method") && message["method"].is_string();
    const bool hasId = message.contains("id") && !message["id"].is_null();

    if (!hasMethod) {
        // A response to a server request; the server sends none, so there is nothing to match
        return;
    }

    if (!hasId) {
        const std::string method = message["method"].get<std::string>();
        const Json params = message.value("params", Json::object());
        if (method == "notifications/cancelled") {
            if (params.is_object() && params.contains("requestId")) {
                cancelRequest(session->id, params["requestId"]);
            }
        }
        // Other notifications (roots/list_changed, progress...) need nothing
        return;
    }

    // 2026-07-28 (stateless): the request names its version in _meta (the HTTP transport
    // checked its headers already); other requests follow their session's initialize
    const std::string metaVersion = requestProtocolVersion(message);
    const bool modern = session->stateless || !metaVersion.empty();
    ReplyPtr out = reply;
    if (modern) {
        const Json& id = message["id"];
        if (metaVersion != MODERN_PROTOCOL_VERSION) {
            reply->respondError(makeError(id, UNSUPPORTED_PROTOCOL_VERSION, "Unsupported protocol version",
                                          Json{{"supported", supportedProtocolVersions()}, {"requested", metaVersion}}), 400);
            return;
        }
        const Json& meta = message["params"]["_meta"];
        if (!meta.contains(META_CLIENT_CAPABILITIES) || !meta[META_CLIENT_CAPABILITIES].is_object()) {
            reply->respondError(makeError(id, -32602, std::string("Invalid params: _meta needs ") + META_CLIENT_CAPABILITIES +
                                                      " (an object) on every " + MODERN_PROTOCOL_VERSION + " request."), 400);
            return;
        }
        // Who sent it comes with every request now (the Activity window shows it)
        if (meta.contains(META_CLIENT_INFO) && meta[META_CLIENT_INFO].is_object()) {
            const Json& info = meta[META_CLIENT_INFO];
            std::lock_guard<std::mutex> lock(sessionMutex);
            session->protocolVersion = MODERN_PROTOCOL_VERSION;
            if (info.contains("name") && info["name"].is_string()) session->clientName = info["name"].get<std::string>();
            if (info.contains("version") && info["version"].is_string()) session->clientVersion = info["version"].get<std::string>();
        }
        out = std::make_shared<ModernReply>(reply, message["method"].get<std::string>());
    }

    try {
        handleRequest(session, message, out, modern);
    } catch (const std::exception& e) {
        out->respond(makeError(message["id"], -32603, std::string("Internal error: ") + e.what()));
    }
}

void McpServer::handleRequest(const SessionPtr& session, const Json& message, const ReplyPtr& reply, bool modern) {
    const Json id = message["id"];
    const std::string method = message["method"].get<std::string>();
    const Json params = message.contains("params") && message["params"].is_object() ? message["params"] : Json::object();

    // 2026-07-28 has no handshake: server/discover tells what initialize did
    if (method == "server/discover") {
        reply->respond(makeResult(id, Json{
            {"supportedVersions", supportedProtocolVersions()},
            {"capabilities", {{"tools", Json::object()}, {"resources", Json::object()}, {"prompts", Json::object()}}},
            {"_meta", {{META_SERVER_INFO, serverInfo()}}},
            {"instructions", instructionsFor(*session)}
        }));
        return;
    }

    if (method == "initialize" && !modern) {
        std::string requested = params.value("protocolVersion", "");
        std::string version = LEGACY_PROTOCOL_VERSIONS[0];
        for (const char* supported : LEGACY_PROTOCOL_VERSIONS) {
            if (requested == supported) version = supported;
        }
        std::string clientName;
        std::string clientVersion;
        if (params.contains("clientInfo") && params["clientInfo"].is_object()) {
            const Json& info = params["clientInfo"];
            if (info.contains("name") && info["name"].is_string()) clientName = info["name"].get<std::string>();
            if (info.contains("version") && info["version"].is_string()) clientVersion = info["version"].get<std::string>();
        }
        {
            // status() copies sessions on the main thread
            std::lock_guard<std::mutex> lock(sessionMutex);
            session->protocolVersion = version;
            session->clientName = clientName;
            session->clientVersion = clientVersion;
        }
        Json result = {
            {"protocolVersion", version},
            {"capabilities", {
                // tool_groups changes a session's listing
                {"tools", {{"listChanged", true}}},
                {"resources", {{"subscribe", false}, {"listChanged", false}}},
                {"prompts", {{"listChanged", false}}},
                {"logging", Json::object()}
            }},
            {"serverInfo", serverInfo()},
            {"instructions", instructionsFor(*session)}
        };
        std::fprintf(stderr, "[mcp] %s session initialized by %s %s (protocol %s)\n",
                     session->transport.c_str(), clientName.c_str(), clientVersion.c_str(), version.c_str());
        reply->respond(makeResult(id, result));
        return;
    }

    if (method == "ping") {
        reply->respond(makeResult(id, Json::object()));
        return;
    }

    if (method == "tools/list") {
        reply->respond(makeResult(id, listTools(session, params)));
        return;
    }

    if (method == "tools/call") {
        handleToolsCall(session, id, params, reply);
        return;
    }

    if (method == "resources/list") {
        Json resources = Json::array();
        for (const ResourceDef& def : reg.resources()) {
            Json item = {{"uri", def.uri}, {"name", def.name}};
            if (!def.title.empty()) item["title"] = def.title;
            if (!def.description.empty()) item["description"] = def.description;
            if (!def.mimeType.empty()) item["mimeType"] = def.mimeType;
            resources.push_back(item);
        }
        reply->respond(makeResult(id, Json{{"resources", resources}}));
        return;
    }

    if (method == "resources/templates/list") {
        Json templates = Json::array();
        for (const ResourceTemplateDef& def : reg.resourceTemplates()) {
            Json item = {{"uriTemplate", def.uriTemplate}, {"name", def.name}};
            if (!def.title.empty()) item["title"] = def.title;
            if (!def.description.empty()) item["description"] = def.description;
            if (!def.mimeType.empty()) item["mimeType"] = def.mimeType;
            templates.push_back(item);
        }
        reply->respond(makeResult(id, Json{{"resourceTemplates", templates}}));
        return;
    }

    if (method == "resources/read") {
        if (!params.contains("uri") || !params["uri"].is_string()) {
            reply->respond(makeError(id, -32602, "resources/read needs a string uri."));
            return;
        }
        const std::string uri = params["uri"].get<std::string>();
        // Resource not found: -32002 up to 2025-11-25, Invalid params since
        const int notFound = modern ? -32602 : -32002;
        if (EditorDialogInfo picker; EditorFilePicker::info(picker)) {
            reply->respond(makeError(id, -32603, dialogBlockMessage(picker)));
            return;
        }
        runOnMainThread([this, id, uri, reply, notFound]() {
            // a resource that writes (project-agents makes AGENTS.md / CLAUDE.md) stays beneath the project
            McpWriteScope writeScope;
            bool found = false;
            const ResourceResult result = reg.readResource(uri, found);
            if (!found) {
                reply->respond(makeError(id, notFound, "Resource not found: " + uri, Json{{"uri", uri}}));
                return;
            }
            if (!result.ok) {
                reply->respond(makeError(id, notFound, result.error.empty() ? "Resource not found: " + uri : result.error,
                                         Json{{"uri", uri}}));
                return;
            }
            Json contents = Json::array();
            for (const ResourceContent& content : result.contents) {
                Json item = {{"uri", content.uri.empty() ? uri : content.uri}};
                if (!content.mimeType.empty()) item["mimeType"] = content.mimeType;
                if (!content.blobBase64.empty() && content.text.empty()) item["blob"] = content.blobBase64;
                else item["text"] = content.text;
                contents.push_back(item);
            }
            reply->respond(makeResult(id, Json{{"contents", contents}}));
        });
        return;
    }

    if (method == "prompts/list") {
        Json prompts = Json::array();
        for (const PromptDef& def : reg.prompts()) {
            Json item = {{"name", def.name}};
            if (!def.title.empty()) item["title"] = def.title;
            if (!def.description.empty()) item["description"] = def.description;
            Json args = Json::array();
            for (const PromptArgument& arg : def.arguments) {
                args.push_back({{"name", arg.name}, {"description", arg.description}, {"required", arg.required}});
            }
            item["arguments"] = args;
            prompts.push_back(item);
        }
        reply->respond(makeResult(id, Json{{"prompts", prompts}}));
        return;
    }

    if (method == "prompts/get") {
        const std::string name = params.value("name", "");
        const PromptDef* def = reg.findPrompt(name);
        if (!def) {
            reply->respond(makeError(id, -32602, "Unknown prompt: " + name));
            return;
        }
        std::map<std::string, std::string> args;
        if (params.contains("arguments") && params["arguments"].is_object()) {
            for (auto it = params["arguments"].begin(); it != params["arguments"].end(); ++it) {
                args[it.key()] = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
            }
        }
        for (const PromptArgument& arg : def->arguments) {
            if (arg.required && !args.count(arg.name)) {
                reply->respond(makeError(id, -32602, "Prompt " + name + " needs argument " + arg.name + "."));
                return;
            }
        }
        if (EditorDialogInfo picker; EditorFilePicker::info(picker)) {
            reply->respond(makeError(id, -32603, dialogBlockMessage(picker)));
            return;
        }
        runOnMainThread([this, id, name, args, reply]() {
            const PromptDef* def = reg.findPrompt(name);
            PromptResult result;
            try {
                result = def->get ? def->get(args) : PromptResult{false, "Prompt has no handler.", "", Json::array()};
            } catch (const std::exception& e) {
                result.ok = false;
                result.error = e.what();
            }
            if (!result.ok) {
                reply->respond(makeError(id, -32602, result.error));
                return;
            }
            Json out = {{"messages", result.messages}};
            if (!result.description.empty()) out["description"] = result.description;
            reply->respond(makeResult(id, out));
        });
        return;
    }

    // 2026-07-28 has no logging/setLevel (the log level comes with each request there); an
    // older session asks for notifications/message with it
    if (method == "logging/setLevel" && !modern) {
        LogLevel level;
        const std::string name = params.contains("level") && params["level"].is_string() ? params["level"].get<std::string>() : "";
        if (!parseMcpLogLevel(name, level)) {
            reply->respond(makeError(id, -32602, "logging/setLevel needs level: debug, info, notice, warning, error, "
                                                 "critical, alert or emergency."));
            return;
        }
        setSessionLogLevel(session, level);
        reply->respond(makeResult(id, Json::object()));
        return;
    }

    if (method == "completion/complete") {
        reply->respond(makeResult(id, Json{{"completion", {{"values", Json::array()}, {"hasMore", false}}}}));
        return;
    }

    // 2026-07-28 answers an unknown method (initialize included) with HTTP 404
    if (modern) {
        const bool initialize = method == "initialize";
        reply->respondError(makeError(id, -32601, "Method not found: " + method +
                                      (initialize ? std::string(" (") + MODERN_PROTOCOL_VERSION +
                                                    " has no initialize: send requests directly, or server/discover)" : ""),
                                      initialize ? Json{{"supported", supportedProtocolVersions()}} : Json()), 404);
        return;
    }
    reply->respond(makeError(id, -32601, "Method not found: " + method));
}

void McpServer::handleToolsCall(const SessionPtr& session, const Json& id, const Json& params, const ReplyPtr& reply) {
    if (!params.contains("name") || !params["name"].is_string()) {
        reply->respond(makeError(id, -32602, "tools/call needs a string name."));
        return;
    }
    const std::string name = params["name"].get<std::string>();
    const ToolDef* def = reg.findTool(name);
    if (!def) {
        reply->respond(makeError(id, -32602, "Unknown tool: " + name + ". Call tools/list for the available tools."));
        return;
    }
    Json args = params.contains("arguments") ? params["arguments"] : Json::object();
    if (args.is_null()) args = Json::object();
    if (!args.is_object()) {
        reply->respond(makeError(id, -32602, "tools/call arguments must be an object."));
        return;
    }

    std::string client;
    std::string clientName;
    {
        std::lock_guard<std::mutex> lock(sessionMutex);
        client = session->clientName.empty() ? session->transport : session->clientName;
        clientName = session->clientName;
    }
    const uint64_t seq = beginActivity(client, name, args);
    const std::string key = inflightKey(session->id, id);
    const bool gated = session->transport == "http";

    // Files changed on disk since the last call (McpAutoSync): the report the main thread
    // writes before the handler runs, and when the handler started (unset: it never ran)
    struct AutoSyncState {
        Json report;
        std::chrono::system_clock::time_point start{};
        bool settle = true;
    };
    auto autoSync = std::make_shared<AutoSyncState>();
    autoSync->settle = mayChange(*def, args);

    auto call = std::make_shared<McpCall>(*this, name, args,
        [this, id, reply, seq, key, autoSync](McpCall& done, const ToolResult& result) {
            {
                std::lock_guard<std::mutex> lock(inflightMutex);
                inflight.erase(key);
                inflightReplies.erase(key);
            }
            endActivity(seq, result, done.elapsedMs(), done.cancelled());
            trace("done", done.tool() + " " + key + (done.cancelled() ? " cancelled" : result.isError ? " error" : " ok"));
            // A cancelled request gets no response (MCP cancellation)
            std::function<void()> respond;
            if (!done.cancelled()) {
                Json out = result.toJson();
                McpAutoSync::attach(out, autoSync->report);
                capResultSize(out);
                auto response = std::make_shared<Json>(makeResult(id, std::move(out)));
                respond = [reply, response]() { reply->respond(std::move(*response)); };
            } else {
                respond = [reply]() { reply->cancelled(); };
            }
            // What the tool wrote is its own, not an external change: settled on the main thread
            // once the handler has returned, and only then answered
            if (autoSync->settle && autoSync->start != std::chrono::system_clock::time_point{}) {
                queueSettle({done.tool(), autoSync->start, std::move(respond)});
            } else {
                respond();
            }
        });

    call->setSession(session);
    call->setNotifier([reply](const Json& notification) {
        if (!reply->streamsNotifications()) return false;
        reply->notify(notification);
        return true;
    });
    call->setClient(McpUndo::clientName(clientName), id.dump());
    call->setChangesGated(gated);

    if (params.contains("_meta") && params["_meta"].is_object() && params["_meta"].contains("progressToken")) {
        const Json token = params["_meta"]["progressToken"];
        auto last = std::make_shared<double>(0.0);
        call->setProgressSink([reply, token, last](double done, double total, const std::string& message) {
            // progress must not go backwards
            *last = std::max(*last, done);
            Json p = {{"progressToken", token}, {"progress", *last}};
            if (total > 0.0) p["total"] = total;
            if (!message.empty()) p["message"] = message;
            reply->notify(Json{{"jsonrpc", "2.0"}, {"method", "notifications/progress"}, {"params", p}});
        });
    }

    {
        std::lock_guard<std::mutex> lock(inflightMutex);
        inflight[key] = call;
        inflightReplies[key] = reply;
    }

    // a tool that must answer while the main thread is stuck: right here, on this thread
    if (def->offMainThread) {
        trace("run", name + " " + key + " (off main)");
        try {
            def->handler(call);
        } catch (const std::exception& e) {
            call->fail(std::string("Tool ") + name + " failed: " + e.what());
        }
        if (!call->completed()) call->fail("Internal error: tool " + name + " returned without a result.");
        return;
    }

    // Checked after the call is in inflight: a picker that opens from now on finds it there
    if (EditorDialogInfo picker; EditorFilePicker::info(picker)) {
        answerDuringFilePicker(call, picker);
        return;
    }

    trace("queued", name + " " + key);
    runOnMainThread([this, call, name, key, autoSync]() {
        // refused while it waited: a file picker opened (onFilePickerOpened)
        if (call->completed()) return;
        call->markStarted();
        trace("run", name + " " + key);
        if (!reg.findTool(name)) {
            call->fail("Unknown tool: " + name);
            return;
        }
        // Pick up files the agent edited on disk first (may wait for shader checks)
        McpAutoSync::get().beforeCall(*ctx, call, name, [this, call, name, autoSync](Json report) {
            autoSync->report = std::move(report);
            const ToolDef* def = reg.findTool(name);
            if (!def) {
                call->fail("Unknown tool: " + name);
                return;
            }
            autoSync->start = std::chrono::system_clock::now();
            if (autoSync->settle) McpAutoSync::get().callStarted();
            {
                HandlerOnStack onStack(handlersOnStack);
                execute(call, *def);
            }
            // the handler has returned: a call it completed is settled and answered in this frame
            runSettles();
        });
    });
}

void McpServer::runOnMainThread(std::function<void()> task) {
    if (!appPtr) return;
    appPtr->enqueueMainThreadTask(std::move(task));
}

void McpServer::queueSettle(Settle settle) {
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(settleMutex);
        if (!settlesClosed) {
            settles.push_back(std::move(settle));
            queued = true;
        }
    }
    if (!queued) {
        settle.respond();
        return;
    }
    // (checked after the push: onFilePickerOpened answers what was queued before it)
    if (EditorFilePicker::isOpen()) answerSettlesNow();
    // A handler or deferred callback on this stack runs the queue once it has returned; a completion
    // anywhere else (a job's thread, another main-thread task) by a task of its own
    if (appPtr->isMainThread() && handlersOnStack > 0) return;
    runOnMainThread([this]() { runSettles(); });
}

void McpServer::runSettles() {
    if (handlersOnStack > 0) return;
    std::vector<Settle> pending;
    {
        std::lock_guard<std::mutex> lock(settleMutex);
        pending.swap(settles);
    }
    for (Settle& settle : pending) {
        // whatever happens to the settle, the client gets its answer
        try {
            if (ctx) McpAutoSync::get().afterCall(*ctx, settle.tool, settle.start);
        } catch (const std::exception& e) {
            Out::error("MCP auto-sync: the settle after %s failed: %s", settle.tool.c_str(), e.what());
        }
        if (settle.respond) settle.respond();
    }
}

void McpServer::answerSettlesNow() {
    std::vector<std::function<void()>> answers;
    {
        std::lock_guard<std::mutex> lock(settleMutex);
        for (Settle& settle : settles) {
            if (!settle.respond) continue;
            answers.push_back(std::move(settle.respond));
            settle.respond = nullptr;
        }
    }
    for (const std::function<void()>& answer : answers) answer();
}

void McpServer::answerDuringFilePicker(const CallPtr& call, const EditorDialogInfo& picker) {
    const std::string& tool = call->tool();
    if (tool == "editor_status") {
        call->ok("Open dialog: \"" + picker.title + "\". Nothing else runs until it is answered.", Json{{"dialog", dialogJson(picker)}});
        return;
    }
    if (tool != "respond_dialog") {
        call->fail(dialogBlockMessage(picker) + " Nothing runs until it is answered.", Json{{"dialog", dialogJson(picker)}});
        return;
    }
    std::string error;
    if (!answerFilePicker(call->args(), error)) {
        call->fail(error, Json{{"dialog", dialogJson(picker)}});
        return;
    }
    // what the code that opened the picker did with the answer takes the main thread's next frames
    const std::string title = picker.title;
    const std::string choice = call->args().value("choice", "");
    runOnMainThread([call, title, choice]() {
        call->afterFrames(2, [call, title, choice]() { reportDialogAnswer(call, title, choice); });
    });
}

void McpServer::onFilePickerOpened() {
    // their settles need this thread, which the picker now holds
    answerSettlesNow();
    EditorDialogInfo picker;
    if (!EditorFilePicker::info(picker)) return;
    const Json data = Json{{"dialog", dialogJson(picker)}};
    const std::string refused = dialogBlockMessage(picker) + " This call was stopped by it: retry it after answering.";

    // A client that sends one call at a time would never get to answer while it waits for these.
    // Calls in the main thread's queue have not begun: nothing of them has happened.
    std::vector<CallPtr> calls;
    {
        std::lock_guard<std::mutex> lock(inflightMutex);
        for (const auto& [key, weak] : inflight) {
            if (CallPtr call = weak.lock()) calls.push_back(call);
        }
    }
    for (const CallPtr& call : calls) {
        const ToolDef* def = reg.findTool(call->tool());
        if (def && !def->offMainThread && !call->started()) call->fail(refused, data);
    }
    // The tool that opened it (run_command, a menu item) only returns when it is answered, which its
    // client would wait for: it replies now, with what to answer
    if (!runningHandlers.empty()) {
        const CallPtr opener = runningHandlers.back();
        if (!opener->completed()) {
            opener->ok(dialogBlockMessage(picker), data);
        }
    }
    // Those waiting for frames that will not come until it closes: respond_dialog replies now,
    // reads and controls stop; an edit's continuation goes on after the picker, and so does a batch
    const std::vector<CallPtr> waiting = [this]() {
        std::vector<CallPtr> out;
        for (const Deferred& item : deferred) out.push_back(item.call);
        return out;
    }();
    for (const CallPtr& call : waiting) {
        if (call->completed()) continue;
        if (std::function<void()> early = call->takeOnFilePicker()) {
            early();
            continue;
        }
        const ToolDef* def = reg.findTool(call->tool());
        if (def && call->tool() != "batch" && (def->annotations.readOnly || effectiveActivity(*def) == ToolActivity::Control)) {
            call->fail(refused, data);
        }
    }
}

std::string McpServer::blockingDialogMessage() const {
    if (!appPtr) return {};
    EditorDialogInfo dialog;
    if (!appPtr->getOpenDialog(dialog)) return {};
    return dialogBlockMessage(dialog);
}

namespace {

// The play state a tool that needs Edit mode meets, for its refusal
std::string playStateText(const Project& project) {
    for (const SceneProject& scene : project.getScenes()) {
        if (scene.playState == ScenePlayState::LOADING) return "Play mode is starting";
        if (scene.playState == ScenePlayState::CANCELLING) return "Play mode is stopping";
    }
    return "a scene is playing";
}

// How long a call waits for a scene save the editor started; JASPER_TEST_SAVE_HOLD_MS shortens it
// for the tests of the timeout
int saveHoldMs() {
    static const int ms = []() {
        const char* value = std::getenv("JASPER_TEST_SAVE_HOLD_MS");
        const int parsed = value ? std::atoi(value) : 0;
        return parsed > 0 ? parsed : 60000;
    }();
    return ms;
}

} // namespace

bool McpServer::mayChange(const ToolDef& def, const Json& args) {
    return !def.annotations.readOnly || (def.writesFiles && args.is_object() && def.writesFiles(args));
}

ToolActivity McpServer::effectiveActivity(const ToolDef& def) {
    if (def.activity != ToolActivity::Auto) return def.activity;
    return def.annotations.readOnly ? ToolActivity::Read : ToolActivity::Edit;
}

std::string McpServer::busyRefusal(ToolActivity activity, Json* info, bool forAgent) const {
    // Which exclusive holders conflict with the activity
    bool vsExport = false;
    bool vsCompile = false;
    const char* what = "This";
    switch (activity) {
        case ToolActivity::Auto:
        case ToolActivity::Read:
        case ToolActivity::Control:
            return {};
        case ToolActivity::Edit:
            vsExport = true;
            what = "Editing the project";
            break;
        case ToolActivity::ScriptCompile:
            vsExport = true;
            what = "Compiling C++ scripts";
            break;
        case ToolActivity::PlayStart:
            vsExport = vsCompile = true;
            what = "Starting Play mode";
            break;
        case ToolActivity::Build:
            vsExport = vsCompile = true;
            what = "Starting a build";
            break;
        case ToolActivity::ProjectSwitch:
            vsExport = vsCompile = true;
            what = "Switching projects";
            break;
    }

    auto describe = [&](const JobPtr& job, const char* holds) {
        if (info) {
            *info = Json{{"job_id", job->id()}, {"kind", job->kind()}, {"title", job->title()},
                         {"holds", jobBusyName(job->busy())}};
        }
        std::string text = std::string(what) + " is refused while job " + job->id() + " (" + job->title() +
                           ") runs: it " + holds + ".";
        if (forAgent) {
            text += " Wait for it: poll get_job with job_id \"" + job->id() +
                    "\" until its status is no longer running (or stop it with cancel_job), then retry.";
        } else {
            text += " Wait for the agent's job to finish (or cancel it from the MCP window), then try again.";
        }
        return text;
    };

    if (vsExport) {
        if (JobPtr job = jobList.runningBusy(JobBusy::Export)) {
            return describe(job, "reads the project from a worker thread and an edit now would race it");
        }
        if (appPtr && appPtr->isExportRunning()) {
            if (info) *info = Json{{"job_id", nullptr}, {"kind", "editor_export"}, {"title", "Build Settings export"}, {"holds", "export"}};
            return std::string(what) + " is refused while the export started in the editor's Build Settings window runs "
                   "(it reads the project from a worker thread)." +
                   (forAgent ? " Retry once a human has finished it (exports usually take a minute or more)." : "");
        }
    }
    if (vsCompile) {
        if (JobPtr job = jobList.runningBusy(JobBusy::ScriptCompile)) {
            return describe(job, "builds the C++ scripts in the project's build directory, which Play and builds use too");
        }
    }
    // Play's startup took the scenes (their saves, generated sources and the snapshot Stop puts
    // the played scene back from) and compiles from them: an edit now would be built without,
    // or lost on Stop; another project would pull the scenes from under it
    if ((activity == ToolActivity::Edit || activity == ToolActivity::ProjectSwitch) && appPtr) {
        if (const Project* project = appPtr->getProject(); project && project->isPlayStarting()) {
            JobPtr playJob;
            for (const JobPtr& job : jobList.list()) {
                if (job->kind() == "play" && job->status() == JobStatus::Running) {
                    playJob = job;
                    break;
                }
            }
            if (info) {
                *info = Json{{"job_id", playJob ? Json(playJob->id()) : Json(nullptr)}, {"kind", "play_startup"},
                             {"title", "Play mode startup"}, {"holds", "play_startup"}};
            }
            const std::string subject = activity == ToolActivity::ProjectSwitch ? std::string(what)
                                                                                : std::string("This call (it can change the project)");
            std::string text = subject + " is refused while Play mode is starting (shaders and scripts are being "
                               "built from the scenes as Play read them).";
            if (!forAgent) {
                return text + " Wait for Play mode to start or stop it, then try again.";
            }
            text += playJob ? " Wait for it: poll get_job with job_id \"" + playJob->id() + "\" until its status is no longer "
                              "running (or get_play_state until playing or stopped)"
                            : " Wait for it: poll get_play_state until the state is playing or stopped";
            return text + ", or stop it with play {action: \"stop\"}, then retry (while playing, change the running game).";
        }
    }
    return {};
}

bool McpServer::refuseIfBusy(const CallPtr& call, ToolActivity activity) {
    Json info;
    const std::string refusal = busyRefusal(activity, &info);
    if (refusal.empty()) return false;
    call->fail(refusal, Json{{"busy", info}});
    return true;
}

void McpServer::execute(const CallPtr& call, const ToolDef& def) {
    App& app = *appPtr;
    if (call->cancelled()) {
        call->fail("Cancelled.");
        return;
    }
    // Checked when the call runs (changes may have been turned off while it waited), with its
    // arguments: a read-only tool may still write a file on request (screenshot save_path)
    if (call->changesGated() && !httpChangesAllowed.load() && mayChange(def, call->args())) {
        call->fail("Changes are turned off for MCP clients over HTTP in the JasperEngine editor (MCP window: "
                   "\"Allow changes\" is off), so only the read-only tools run, without arguments that write "
                   "files, and this " + def.name + " call changes the project. Ask the user to allow changes, "
                   "then list the tools again.");
        return;
    }
    // the Project Hub over no project (S27 R16): only the project tools and the server's own run
    const std::string unavailable = app.projectUnavailableReason();
    if (!unavailable.empty()) {
        static const char* const kWithoutProject[] = {"create_project", "open_project", "import_project", "list_project_templates", "clear_project_leftovers",
                                                      "editor_status", "get_job", "list_jobs", "cancel_job", "tool_groups",
                                                      "read_console", "get_editor_health"};
        const bool allowed = std::any_of(std::begin(kWithoutProject), std::end(kWithoutProject),
                                         [&def](const char* name) { return def.name == name; });
        if (!allowed) {
            call->fail(unavailable);
            return;
        }
    }
    Project* project = app.getProject();
    // a scene save the editor started (Ctrl+S, Save All, Save As): the call is valid, so it waits
    // for the save, as at the window, instead of reading or changing what the save worker reads
    if (project && project->isAnySceneSaving() && !project->isPlaySessionActive() &&
        (mayChange(def, call->args()) || !def.allowedWhilePlaying)) {
        const ToolDef* waiting = &def;
        const double started = call->elapsedMs();
        call->waitUntil([this]() {
            const Project* current = appPtr->getProject();
            return !current || !current->isAnySceneSaving();
        }, saveHoldMs(), [this, call, waiting, started](bool timedOut) {
            if (timedOut) {
                call->fail("A scene save started in the editor is still running after " + std::to_string(saveHoldMs() / 1000) +
                           " s; nothing was changed. Retry when editor_status shows no scene saving.",
                           Json{{"busy", {{"kind", "scene_save"}}}});
                return;
            }
            call->addSaveWait(call->elapsedMs() - started);
            execute(call, *waiting);
        });
        return;
    }
    if (!def.allowedWhilePlaying && project->isAnyScenePlaying()) {
        call->fail("This tool is not available while " + playStateText(*project) + ". Stop Play mode first "
                   "(play with action \"stop\"), then retry.");
        return;
    }
    if (refuseIfBusy(call, effectiveActivity(def))) return;

    // An info alert (or an error box) left from earlier only needs OK; anything else needs an
    // answer first, except a modal window built to keep concurrent edits (Project Settings)
    std::string notes;
    EditorDialogInfo dialog;
    if (!def.annotations.readOnly && app.getOpenDialog(dialog) && dialog.blocksEdits) {
        if ((dialog.kind == "alert" || dialog.kind == "modal") && dialog.choices.size() == 1 && dialog.choices[0] == "ok" &&
            dialog.fields.empty() && dialog.disabledChoices.empty()) {
            std::string error;
            app.answerDialog("ok", {}, error);
            notes += "\n(Dismissed an editor " + std::string(dialog.kind == "alert" ? "alert" : "message") + " shown earlier: " +
                     dialog.title + (dialog.message.empty() ? std::string() : ": " + dialog.message) + ")";
        } else if (def.name != "respond_dialog" && def.name != "batch" &&
                   // focus_window's page and close act on Preferences itself
                   !(def.name == "focus_window" && dialog.title == "Preferences" && call->args().value("window", "") == "preferences")) {
            call->fail(blockingDialogMessage() + " Edits are refused until it is answered.",
                       Json{{"dialog", dialogJson(dialog)}});
            return;
        }
    }

    // An edit tool's undo steps are the agent's (S24 R24): attributed while its handler and
    // its deferred continuations run, folded into one step per history when it replies, and
    // named in the result (structured.undo). Read-only and control tools (batch) are not.
    const ToolActivity activity = effectiveActivity(def);
    if ((activity != ToolActivity::Read && activity != ToolActivity::Control) || def.attributesUndo) {
        UndoOrigin origin;
        origin.kind = UndoOriginKind::Agent;
        origin.client = call->client().empty() ? std::string("Agent") : call->client();
        origin.callId = call->requestId();
        origin.tool = def.name;
        origin.call = call->serial();
        call->setUndoOrigin(std::move(origin));
    }

    // Alerts raised by the tool are part of its answer: the agent can't see the window
    const uint64_t alertSerial = app.getAlertSerial();
    const std::string toolTitle = def.title.empty() ? def.name : def.title;
    std::weak_ptr<McpCall> weakCall = call;
    call->setFinalizer([this, alertSerial, notes, toolTitle, weakCall](ToolResult& result) {
        result.text += notes;
        if (CallPtr done = weakCall.lock(); done && done->saveWait() > 0.0) {
            const long long ms = std::llround(done->saveWait());
            result.text += "\n(Waited " + std::to_string(ms) + " ms for a scene save started in the editor.)";
            if (result.structured.is_null()) result.structured = Json::object();
            if (result.structured.is_object()) result.structured["waited_for_save_ms"] = ms;
        }
        if (!appPtr || !appPtr->isMainThread()) return;
        if (CallPtr done = weakCall.lock(); done && done->attributed()) {
            Json undo = McpUndo::foldCall(*done, toolTitle);
            if (!undo.is_null()) {
                if (result.structured.is_null()) result.structured = Json::object();
                if (result.structured.is_object()) result.structured["undo"] = undo;
            }
        }
        App& app = *appPtr;
        EditorDialogInfo raised;
        if (app.getAlertSerial() == alertSerial || !app.getOpenDialog(raised) || raised.kind != "alert") return;
        if (raised.choices.size() == 1) {
            // An info alert only needs OK; mostly errors the tool reported this way
            std::string error;
            app.answerDialog("ok", {}, error);
            result.text += "\nEditor alert: " + raised.title + ": " + raised.message;
        } else {
            result.text += "\nThe editor now asks \"" + raised.title + "\": " + raised.message +
                           " Answer with respond_dialog (choices:";
            for (const std::string& choice : raised.choices) result.text += " " + choice;
            result.text += ").";
        }
    });

    // Each call (and each step of a batch) starts and ends with packed component arrays: the
    // holes of entities destroyed before are closed between steps (ComponentArray)
    const bool onMainThread = app.isMainThread();
    if (onMainThread && app.getProject()) app.getProject()->compactAllComponents();

    // every file the tool writes stays beneath the project (util/SafeWrite.h): FileUtils' writes,
    // and the commands it makes (their undo and redo too); outside the project nothing but the
    // editor's own folders, except for a tool that switches projects (its paths are explicit)
    McpWriteScope writeScope(McpWriteScope::projectRoots(), effectiveActivity(def) != ToolActivity::ProjectSwitch);
    try {
        std::optional<UndoOriginScope> attribution;
        if (call->attributed()) attribution.emplace(call->getUndoOrigin());
        // an agent's edit in the frame's profile (benches tell an edit's frame from others: S32 R22)
        JE_PROFILE_SCOPE("MCP tool");
        if (onMainThread) FrameTimes::markHostWork();
        struct RunningHandler {
            std::vector<CallPtr>& stack;
            RunningHandler(std::vector<CallPtr>& s, const CallPtr& c) : stack(s) { stack.push_back(c); }
            ~RunningHandler() { stack.pop_back(); }
        } running(runningHandlers, call);
        def.handler(call);
    } catch (const Json::type_error& e) {
        // an argument a handler read without checking its type: still an error the agent can fix
        call->fail(std::string("Tool ") + def.name + ": an argument has the wrong type (" + e.what() +
                   "); its inputSchema (tools/list) gives each argument's type.");
    } catch (const std::exception& e) {
        call->fail(std::string("Tool ") + def.name + " failed: " + e.what());
    } catch (...) {
        call->fail(std::string("Tool ") + def.name + " failed with an unknown error.");
    }

    if (onMainThread && appPtr && appPtr->getProject()) appPtr->getProject()->compactAllComponents();

    if (!call->completed() && !hasDeferred(call.get()) && !call->detached()) {
        call->fail("Internal error: tool " + def.name + " returned without a result.");
    }
}

void McpServer::runNested(const CallPtr& parent, const std::string& tool, const Json& args,
                          std::function<void(const ToolResult&)> done) {
    const ToolDef* def = reg.findTool(tool);
    if (!def) {
        done(ToolResult::error("Unknown tool: " + tool + "."));
        return;
    }
    std::weak_ptr<McpCall> weakParent = parent;
    auto call = std::make_shared<McpCall>(*this, tool, args,
        [done](McpCall&, const ToolResult& result) {
            done(result);
        });
    call->setProgressSink([weakParent, tool](double progressDone, double total, const std::string& message) {
        if (CallPtr p = weakParent.lock()) p->progress(progressDone, total, tool + ": " + message);
    });
    call->setParent(parent);
    call->setClient(parent->client(), parent->requestId());
    // A batch can't run what its own client may not
    call->setChangesGated(parent->changesGated());
    execute(call, *def);
}

void McpServer::runLocal(const std::string& tool, const Json& args, const std::string& client,
                         std::function<void(const ToolResult&)> done) {
    const ToolDef* def = reg.findTool(tool);
    if (!def || !started) {
        done(ToolResult::error(def ? "The MCP server is not running." : "Unknown tool: " + tool + "."));
        return;
    }
    auto call = std::make_shared<McpCall>(*this, tool, args, [done](McpCall&, const ToolResult& result) { done(result); });
    call->setClient(client, std::string());
    execute(call, *def);
}

// ---------------------------------------------------------------- deferral

void McpServer::addDeferred(Deferred item) {
    deferred.push_back(std::move(item));
    if (appPtr) appPtr->requestRedraw();
}

bool McpServer::hasDeferred(const McpCall* call) const {
    for (const Deferred& item : deferred) {
        if (item.call.get() == call) return true;
    }
    return false;
}

std::string McpServer::currentProjectPath() const {
    if (!appPtr || !appPtr->getProject()) return {};
    return appPtr->getProject()->getProjectPath().generic_string();
}

void McpServer::tick() {
    if (!started) return;
    if (ctx) McpAutoSync::get().prepare(*ctx);
    if (instanceRecord.active()) instanceRecord.projectChanged(currentProjectPath());
    if (deferred.empty()) return;

    // Callbacks may defer again (chained waits), so work on a snapshot
    std::vector<Deferred> current;
    current.swap(deferred);
    std::vector<Deferred> keep;
    const auto now = std::chrono::steady_clock::now();
    handlersOnStack++;

    for (Deferred& item : current) {
        const CallPtr& call = item.call;
        if (call->completed()) continue;
        if (call->cancelled() && !item.done) {
            call->fail("Cancelled.");
            continue;
        }
        // a continuation writes beneath the project as the tool's run did
        McpWriteScope writeScope;
        try {
            // a continuation of an attributed call is the agent's too
            std::optional<UndoOriginScope> attribution;
            if (call->attributed()) attribution.emplace(call->getUndoOrigin());
            if (item.frames >= 0) {
                if (item.frames > 0) {
                    item.frames--;
                    keep.push_back(std::move(item));
                    continue;
                }
                if (item.fn) item.fn();
            } else {
                bool finished = item.done ? item.done() : true;
                bool timedOut = false;
                if (!finished && item.hasDeadline && now >= item.deadline) {
                    timedOut = true;
                }
                if (!finished && !timedOut) {
                    if (call->cancelled()) {
                        call->fail("Cancelled.");
                    } else {
                        keep.push_back(std::move(item));
                    }
                    continue;
                }
                if (item.then) item.then(timedOut);
            }
        } catch (const std::exception& e) {
            call->fail(std::string("Tool ") + call->tool() + " failed: " + e.what());
            continue;
        } catch (...) {
            call->fail(std::string("Tool ") + call->tool() + " failed with an unknown error.");
            continue;
        }
        if (!call->completed() && !call->detached() && !hasDeferred(call.get())) {
            bool stillQueued = false;
            for (const Deferred& other : keep) {
                if (other.call == call) stillQueued = true;
            }
            if (!stillQueued) call->fail("Internal error: tool " + call->tool() + " deferred without a result.");
        }
    }

    // New deferrals added by callbacks went to `deferred`
    for (Deferred& item : keep) deferred.push_back(std::move(item));

    // Calls the callbacks above completed: settled and answered now that they have returned
    handlersOnStack--;
    runSettles();

    // Keep the loop drawing frames (instead of idling) while anything waits
    if (!deferred.empty() && appPtr) appPtr->requestRedraw();
}
