// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpServer.h"

#include "McpContext.h"
#include "McpTransportHttp.h"
#include "McpTransportStdio.h"
#include "tools/McpTools.h"

#include "App.h"
#include "AppSettings.h"
#include "EditorVersion.h"
#include "Out.h"

#include <algorithm>
#include <csignal>
#include <cstdio>

using namespace jasperengine::editor;
using namespace jasperengine::editor::mcp;

namespace {

// Newest first: an unknown client version gets the newest
const char* const SUPPORTED_PROTOCOL_VERSIONS[] = {
    "2025-11-25",
    "2025-06-18",
    "2025-03-26",
    "2024-11-05"
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

const char* playingRefusal() {
    return "This tool is not available while a scene is playing. Stop Play mode first "
           "(play with action \"stop\"), then retry.";
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

    registerAllTools();

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
    registerDocsTools(reg);
    registerBuildTools(reg);
    registerWorldTools(reg);
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
}

void McpServer::setHttpEnabled(bool enabled, int port) {
    if (!httpTransport) {
        httpTransport = std::make_unique<McpTransportHttp>(*this);
    }
    if (!enabled) {
        httpTransport->stop();
        return;
    }
    if (httpTransport->isRunning() && httpTransport->port() == port) return;
    httpTransport->stop();
    std::string error;
    if (httpTransport->start(port, error)) {
        Out::info("MCP server listening on http://127.0.0.1:%d/mcp", port);
    } else {
        Out::error("MCP HTTP server could not start on port %d: %s", port, error.c_str());
    }
}

McpServerStatus McpServer::status() const {
    McpServerStatus out;
    out.started = started;
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

// ---------------------------------------------------------------- JSON-RPC

Json McpServer::makeError(const Json& id, int code, const std::string& message, Json data) {
    Json error = {{"code", code}, {"message", message}};
    if (!data.is_null()) error["data"] = std::move(data);
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", error}};
}

Json McpServer::makeResult(const Json& id, Json result) {
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
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
        if (method == "notifications/initialized") {
            session->initialized = true;
        } else if (method == "notifications/cancelled") {
            if (params.is_object() && params.contains("requestId")) {
                std::lock_guard<std::mutex> lock(inflightMutex);
                auto it = inflight.find(inflightKey(session->id, params["requestId"]));
                if (it != inflight.end()) {
                    if (CallPtr call = it->second.lock()) call->requestCancel();
                }
            }
        }
        // Other notifications (roots/list_changed, progress...) need nothing
        return;
    }

    try {
        handleRequest(session, message, reply);
    } catch (const std::exception& e) {
        reply->respond(makeError(message["id"], -32603, std::string("Internal error: ") + e.what()));
    }
}

void McpServer::handleRequest(const SessionPtr& session, const Json& message, const ReplyPtr& reply) {
    const Json id = message["id"];
    const std::string method = message["method"].get<std::string>();
    const Json params = message.contains("params") && message["params"].is_object() ? message["params"] : Json::object();

    if (method == "initialize") {
        std::string requested = params.value("protocolVersion", "");
        std::string version = SUPPORTED_PROTOCOL_VERSIONS[0];
        for (const char* supported : SUPPORTED_PROTOCOL_VERSIONS) {
            if (requested == supported) version = supported;
        }
        session->protocolVersion = version;
        if (params.contains("clientInfo") && params["clientInfo"].is_object()) {
            session->clientName = params["clientInfo"].value("name", "");
            session->clientVersion = params["clientInfo"].value("version", "");
        }
        Json result = {
            {"protocolVersion", version},
            {"capabilities", {
                {"tools", {{"listChanged", false}}},
                {"resources", {{"subscribe", false}, {"listChanged", false}}},
                {"prompts", {{"listChanged", false}}},
                {"logging", Json::object()}
            }},
            {"serverInfo", {
                {"name", SERVER_NAME},
                {"title", "JasperEngine Editor"},
                {"version", JASPERENGINE_EDITOR_VERSION}
            }},
            {"instructions", mcpServerInstructions()}
        };
        std::fprintf(stderr, "[mcp] %s session initialized by %s %s (protocol %s)\n",
                     session->transport.c_str(), session->clientName.c_str(),
                     session->clientVersion.c_str(), version.c_str());
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
        runOnMainThread([this, id, uri, reply]() {
            ResourceResult result;
            try {
                std::map<std::string, std::string> vars;
                if (const ResourceDef* def = reg.findResource(uri)) {
                    result = def->read ? def->read(uri) : ResourceResult{false, "Resource has no reader.", {}};
                } else if (const ResourceTemplateDef* tmpl = reg.matchResourceTemplate(uri, vars)) {
                    result = tmpl->read ? tmpl->read(uri, vars) : ResourceResult{false, "Resource has no reader.", {}};
                } else {
                    reply->respond(makeError(id, -32002, "Resource not found: " + uri, Json{{"uri", uri}}));
                    return;
                }
            } catch (const std::exception& e) {
                result.ok = false;
                result.error = e.what();
            }
            if (!result.ok) {
                reply->respond(makeError(id, -32002, result.error.empty() ? "Resource not found: " + uri : result.error,
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

    if (method == "logging/setLevel") {
        session->logLevel = params.value("level", "info");
        reply->respond(makeResult(id, Json::object()));
        return;
    }

    if (method == "completion/complete") {
        reply->respond(makeResult(id, Json{{"completion", {{"values", Json::array()}, {"hasMore", false}}}}));
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

    const std::string client = session->clientName.empty() ? session->transport : session->clientName;
    const uint64_t seq = beginActivity(client, name, args);
    const std::string key = inflightKey(session->id, id);

    auto call = std::make_shared<McpCall>(*this, name, args,
        [this, id, reply, seq, key](McpCall& done, const ToolResult& result) {
            {
                std::lock_guard<std::mutex> lock(inflightMutex);
                inflight.erase(key);
            }
            endActivity(seq, result, done.elapsedMs(), done.cancelled());
            // A cancelled request gets no response (MCP cancellation)
            if (!done.cancelled()) {
                reply->respond(makeResult(id, result.toJson()));
            } else {
                reply->cancelled();
            }
        });

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

    runOnMainThread([this, call, name]() {
        const ToolDef* def = reg.findTool(name);
        if (!def) {
            call->fail("Unknown tool: " + name);
            return;
        }
        execute(call, *def);
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

void McpServer::execute(const CallPtr& call, const ToolDef& def) {
    App& app = *appPtr;
    if (call->cancelled()) {
        call->fail("Cancelled.");
        return;
    }
    if (!def.allowedWhilePlaying && app.getProject()->isAnyScenePlaying()) {
        call->fail(playingRefusal());
        return;
    }

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
    if (parent->cancelled()) call->requestCancel();
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

size_t McpServer::pendingCount() const {
    return deferred.size();
}

void McpServer::tick() {
    if (!started || deferred.empty()) return;

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
