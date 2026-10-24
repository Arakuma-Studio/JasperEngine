// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpServer.h"

#include "McpAutoSync.h"
#include "McpContext.h"
#include "McpTransportHttp.h"
#include "McpTransportStdio.h"
#include "tools/McpTools.h"

#include "App.h"
#include "AppSettings.h"
#include "EditorVersion.h"
#include "Out.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstdio>

using namespace jasperengine::editor;
using namespace jasperengine::editor::mcp;

namespace {

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
    appPtr = &app;
    ctx = std::make_unique<McpContext>(app, *this);
    McpAutoSync::get().serverStarted();
    installPoseSyncHook(*ctx);

    registerAllTools();

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

void McpServer::registerAllTools() {
    // Fixed order (tools/list order)
    registerCoreTools(reg);
    registerProjectTools(reg);
    registerSceneTools(reg);
    registerEntityTools(reg);
    registerComponentTools(reg);
    registerPrefabTools(reg);
    registerPlayTools(reg);
    registerViewTools(reg);
    registerConsoleTools(reg);
    registerScriptTools(reg);
    registerAssetTools(reg);
    registerSynthesisTools(reg);
    registerDocsTools(reg);
    registerBuildTools(reg);
    registerWorldTools(reg);
    registerLatticeTools(reg);
    registerHtmlUiTools(reg);
    registerUiEffectTools(reg);
    registerPoseTools(reg);
}

void McpServer::stop() {
    if (!started) return;
    if (httpTransport) {
        httpTransport->stop();
    }
    // Calls still waiting on frames would never finish now
    std::vector<Deferred> pending;
    pending.swap(deferred);
    for (Deferred& item : pending) {
        item.call->requestCancel();
        item.call->fail("The editor is shutting down.");
    }
    // Let the answers already given reach the stdio client before the process exits
    if (stdioTransport) stdioTransport->flush(2000);
}

void McpServer::setHttpEnabled(bool enabled, int port) {
    if (!httpTransport) {
        httpTransport = std::make_unique<McpTransportHttp>(*this);
    }
    if (!enabled) {
        httpTransport->stop();
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
    } else {
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

// ---------------------------------------------------------------- sessions

SessionPtr McpServer::createSession(const std::string& transport) {
    auto session = std::make_shared<McpSession>();
    session->transport = transport;
    std::lock_guard<std::mutex> lock(sessionMutex);
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

void McpServer::closeSession(const std::string& id) {
    {
        std::lock_guard<std::mutex> lock(sessionMutex);
        sessions.erase(id);
    }
    // Its pending requests have nobody to answer to
    std::lock_guard<std::mutex> lock(inflightMutex);
    const std::string prefix = id + "|";
    for (auto it = inflight.begin(); it != inflight.end();) {
        if (it->first.compare(0, prefix.size(), prefix) == 0) {
            if (CallPtr call = it->second.lock()) call->requestCancel();
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

// Length of the valid UTF-8 sequence at text[i], or 0 if it is invalid (overlong forms,
// surrogates and code points past U+10FFFF are invalid, as nlohmann::json's parser says)
size_t utf8SequenceLength(const std::string& text, size_t i) {
    const auto byte = [&](size_t k) { return static_cast<unsigned char>(text[k]); };
    const unsigned char c = byte(i);
    if (c < 0x80) return 1;
    size_t length = 0;
    unsigned char low = 0x80, high = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) length = 2;
    else if (c == 0xE0) { length = 3; low = 0xA0; }
    else if (c >= 0xE1 && c <= 0xEC) length = 3;
    else if (c == 0xED) { length = 3; high = 0x9F; }
    else if (c >= 0xEE && c <= 0xEF) length = 3;
    else if (c == 0xF0) { length = 4; low = 0x90; }
    else if (c >= 0xF1 && c <= 0xF3) length = 4;
    else if (c == 0xF4) { length = 4; high = 0x8F; }
    else return 0;
    if (i + length > text.size()) return 0;
    if (byte(i + 1) < low || byte(i + 1) > high) return 0;
    for (size_t k = 2; k < length; k++) {
        if (byte(i + k) < 0x80 || byte(i + k) > 0xBF) return 0;
    }
    return length;
}

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
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        const size_t length = utf8SequenceLength(text, i);
        if (length == 0) {
            out += "\xEF\xBF\xBD";
            i++;
        } else {
            out.append(text, i, length);
            i += length;
        }
    }
    return out;
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
                {"tools", {{"listChanged", false}}},
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
        Json tools = Json::array();
        for (const ToolDef& def : reg.tools()) {
            if (toolHidden(*session, def)) continue;
            tools.push_back(McpRegistry::toolToJson(def));
        }
        reply->respond(makeResult(id, Json{{"tools", tools}}));
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
        runOnMainThread([this, id, uri, reply, notFound]() {
            ResourceResult result;
            try {
                std::map<std::string, std::string> vars;
                if (const ResourceDef* def = reg.findResource(uri)) {
                    result = def->read ? def->read(uri) : ResourceResult{false, "Resource has no reader.", {}};
                } else if (const ResourceTemplateDef* tmpl = reg.matchResourceTemplate(uri, vars)) {
                    result = tmpl->read ? tmpl->read(uri, vars) : ResourceResult{false, "Resource has no reader.", {}};
                } else {
                    reply->respond(makeError(id, notFound, "Resource not found: " + uri, Json{{"uri", uri}}));
                    return;
                }
            } catch (const std::exception& e) {
                result.ok = false;
                result.error = e.what();
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

    // Gone in 2026-07-28 (the log level comes with each request there)
    if (method == "logging/setLevel" && !modern) {
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
    {
        std::lock_guard<std::mutex> lock(sessionMutex);
        client = session->clientName.empty() ? session->transport : session->clientName;
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
            }
            endActivity(seq, result, done.elapsedMs(), done.cancelled());
            trace("done", done.tool() + " " + key + (result.isError ? " error" : " ok"));
            // What the tool wrote is its own, not an external change: settled on the main
            // thread once the handler has returned (before the client's next call runs)
            if (autoSync->settle && autoSync->start != std::chrono::system_clock::time_point{}) {
                const auto start = autoSync->start;
                const auto end = std::chrono::system_clock::now();
                const std::string tool = done.tool();
                runOnMainThread([this, tool, start, end]() {
                    if (ctx) McpAutoSync::get().afterCall(*ctx, tool, start, end);
                });
            }
            // A cancelled request gets no response (MCP cancellation)
            if (!done.cancelled()) {
                Json out = result.toJson();
                McpAutoSync::attach(out, autoSync->report);
                capResultSize(out);
                reply->respond(makeResult(id, std::move(out)));
            } else {
                reply->cancelled();
            }
        });

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
    }

    trace("queued", name + " " + key);
    runOnMainThread([this, call, name, key, autoSync]() {
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
            execute(call, *def);
        });
    });
}

void McpServer::runOnMainThread(std::function<void()> task) {
    if (!appPtr) return;
    appPtr->enqueueMainThreadTask(std::move(task));
}

std::string McpServer::blockingDialogMessage() const {
    if (!appPtr) return {};
    EditorDialogInfo dialog;
    if (!appPtr->getOpenDialog(dialog)) return {};
    std::string text = "The editor is showing the dialog \"" + dialog.title + "\"";
    if (!dialog.message.empty()) text += " (" + dialog.message + ")";
    if (!dialog.choices.empty()) {
        text += ". Answer it with respond_dialog, choice one of:";
        for (const std::string& choice : dialog.choices) text += " " + choice;
    } else {
        text += ", which only a human can close in the editor window";
    }
    return text + ".";
}

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
    if (!def.allowedWhilePlaying && app.getProject()->isAnyScenePlaying()) {
        call->fail("This tool is not available while a scene is playing. Stop Play mode first "
                   "(play with action \"stop\"), then retry.");
        return;
    }
    if (refuseIfBusy(call, effectiveActivity(def))) return;

    // An info alert left from earlier only needs OK; anything else needs an answer first
    std::string notes;
    EditorDialogInfo dialog;
    if (!def.annotations.readOnly && app.getOpenDialog(dialog)) {
        if (dialog.kind == "alert" && dialog.choices.size() == 1 && dialog.choices[0] == "ok") {
            std::string error;
            app.answerDialog("ok", error);
            notes += "\n(Dismissed an editor alert shown earlier: " + dialog.title + ": " + dialog.message + ")";
        } else if (def.name != "respond_dialog" && def.name != "batch") {
            call->fail(blockingDialogMessage() + " Edits are refused until it is answered.",
                       Json{{"dialog", {{"kind", dialog.kind}, {"title", dialog.title}, {"message", dialog.message},
                                        {"choices", dialog.choices}}}});
            return;
        }
    }

    // Alerts raised by the tool are part of its answer: the agent can't see the window
    const uint64_t alertSerial = app.getAlertSerial();
    call->setFinalizer([this, alertSerial, notes](ToolResult& result) {
        result.text += notes;
        if (!appPtr || !appPtr->isMainThread()) return;
        App& app = *appPtr;
        EditorDialogInfo raised;
        if (app.getAlertSerial() == alertSerial || !app.getOpenDialog(raised) || raised.kind != "alert") return;
        if (raised.choices.size() == 1) {
            // An info alert only needs OK; mostly errors the tool reported this way
            std::string error;
            app.answerDialog("ok", error);
            result.text += "\nEditor alert: " + raised.title + ": " + raised.message;
        } else {
            result.text += "\nThe editor now asks \"" + raised.title + "\": " + raised.message +
                           " Answer with respond_dialog (choices:";
            for (const std::string& choice : raised.choices) result.text += " " + choice;
            result.text += ").";
        }
    });

    try {
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
    // A batch can't run what its own client may not
    call->setChangesGated(parent->changesGated());
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

void McpServer::tick() {
    if (!started) return;
    if (ctx) McpAutoSync::get().prepare(*ctx);
    if (deferred.empty()) return;

    // Callbacks may defer again (chained waits), so work on a snapshot
    std::vector<Deferred> current;
    current.swap(deferred);
    std::vector<Deferred> keep;
    const auto now = std::chrono::steady_clock::now();

    for (Deferred& item : current) {
        const CallPtr& call = item.call;
        if (call->completed()) continue;
        if (call->cancelled() && !item.done) {
            call->fail("Cancelled.");
            continue;
        }
        try {
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

    // Keep the loop drawing frames (instead of idling) while anything waits
    if (!deferred.empty() && appPtr) appPtr->requestRedraw();
}
