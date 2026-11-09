// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CallCommand.h"

#include "cli/CommandLine.h"
#include "cli/McpClient.h"
#include "cli/SchemaArgs.h"

#include "EditorVersion.h"
#include "io/AtomicFile.h"
#include "mcp/McpInstances.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpToolGroups.h"
#include "mcp/tools/McpTools.h"
#include "util/Base64.h"
#include "util/FileUtils.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <thread>
#include "Catalog.h"
#include "htmlui/HttpClient.h"
#include "window/widget/InputTextContextMenu.h"
#include "htmlui/compat/css/RewriteSession.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace jasperengine::editor::cli {

namespace {

using mcp::InstanceInfo;

constexpr int DEFAULT_TIMEOUT_SECONDS = 120;
constexpr int DEFAULT_SPAWN_TIMEOUT_SECONDS = 600;
constexpr size_t MAX_ATOMIC_LINES = 200;   // the batch tool's limit (CoreTools.cpp)

std::atomic<bool> interruptFlag{false};

#ifdef _WIN32
BOOL WINAPI onConsoleCtrl(DWORD type) {
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) return FALSE;
    interruptFlag = true;
    return TRUE;
}
#else
void onSigint(int) {
    interruptFlag = true;
}
#endif

// The editor links with the GUI subsystem on Windows: the streams the parent did not redirect
// (pipes and files stay as they are) go to its console
void attachConsole() {
#ifdef _WIN32
    auto usable = [](DWORD which) {
        const HANDLE handle = GetStdHandle(which);
        return handle && handle != INVALID_HANDLE_VALUE && GetFileType(handle) != FILE_TYPE_UNKNOWN;
    };
    const bool out = usable(STD_OUTPUT_HANDLE);
    const bool err = usable(STD_ERROR_HANDLE);
    const bool in = usable(STD_INPUT_HANDLE);
    if ((out && err && in) || !AttachConsole(ATTACH_PARENT_PROCESS)) return;
    FILE* file = nullptr;
    if (!out) (void)freopen_s(&file, "CONOUT$", "w", stdout);
    if (!err) (void)freopen_s(&file, "CONOUT$", "w", stderr);
    if (!in) (void)freopen_s(&file, "CONIN$", "r", stdin);
#endif
}

void installInterruptHandler() {
#ifdef _WIN32
    SetConsoleCtrlHandler(onConsoleCtrl, TRUE);
#else
    std::signal(SIGINT, onSigint);
    std::signal(SIGPIPE, SIG_IGN);
#endif
}

// A failure with its exit code (usage errors are UsageError, editor ones ClientError)
struct Failure {
    Exit code;
    std::string message;
};

const char* codeName(int exit) {
    switch (exit) {
        case Ok: return nullptr;
        case ToolError: return "tool_error";
        case Usage: return "usage";
        case Connection: return "connection";
        default: return "internal";
    }
}

std::string commandName(const char* executableName) {
    return fs::path(executableName ? executableName : "jasperengine-editor").filename().string();
}

// ---------------------------------------------------------------- options

struct Options {
    std::string url;
    int port = 0;
    std::string token;
    bool tokenSet = false;
    std::string project;          // absolute
    double timeout = DEFAULT_TIMEOUT_SECONDS;
    bool json = false;
    bool structured = false;
    bool quiet = false;
    bool dryRun = false;
    std::string imageDir = ".";
    bool waitJob = false;
    bool help = false;
    // batch
    bool continueOnError = false;
    bool atomic = false;
    // list-tools
    std::string group;

    // a connection target was given (the live registry is used for help and listings)
    bool hasTarget() const {
        return !url.empty() || port > 0 || std::getenv("JASPER_MCP_URL") != nullptr || !project.empty();
    }
};

// Parses the globals (and, for batch/list-tools/editors, their own flags) from args[i...];
// stops at the first token that is not a flag (the tool name of call)
void parseOptions(const std::vector<std::string>& args, size_t& i, Options& o, const std::vector<std::string>& allowed) {
    for (; i < args.size(); i++) {
        const std::string& arg = args[i];
        if (arg.rfind("-", 0) != 0 || arg == "-") return;
        std::string name = arg;
        std::string value;
        const size_t eq = arg.find('=');
        const bool hasValue = eq != std::string::npos;
        if (hasValue) {
            name = arg.substr(0, eq);
            value = arg.substr(eq + 1);
        }
        if (name == "-h") name = "--help";
        if (std::find(allowed.begin(), allowed.end(), name) == allowed.end()) {
            std::string message = "unknown option " + name;
            std::vector<std::string> bare;
            for (const std::string& a : allowed) bare.push_back(a);
            const std::vector<std::string> close = closeMatches(name, bare);
            if (!close.empty()) message += " (did you mean " + close[0] + "?)";
            message += ". Options go before the tool name: ";
            for (size_t k = 0; k < allowed.size(); k++) message += (k ? " " : "") + allowed[k];
            throw UsageError(message);
        }
        auto take = [&]() -> std::string {
            if (hasValue) return value;
            if (i + 1 >= args.size()) throw UsageError(name + " needs a value");
            return args[++i];
        };
        auto flag = [&]() {
            if (hasValue) throw UsageError(name + " takes no value");
            return true;
        };
        if (name == "--url") o.url = take();
        else if (name == "--port") {
            const std::string text = take();
            char* end = nullptr;
            const long port = std::strtol(text.c_str(), &end, 10);
            if (!end || *end || port < 1 || port > 65535) throw UsageError("--port takes a port number (1-65535)");
            o.port = static_cast<int>(port);
        } else if (name == "--token") {
            o.token = take();
            o.tokenSet = true;
        } else if (name == "--project") o.project = CommandLine::projectArgPath(take()).string();
        else if (name == "--timeout") {
            const std::string text = take();
            char* end = nullptr;
            const double seconds = std::strtod(text.c_str(), &end);
            if (!end || *end || !(seconds > 0)) throw UsageError("--timeout takes seconds (> 0)");
            o.timeout = seconds;
        } else if (name == "--json") o.json = flag();
        else if (name == "--structured") o.structured = flag();
        else if (name == "--quiet") o.quiet = flag();
        else if (name == "--dry-run") o.dryRun = flag();
        else if (name == "--image-dir") o.imageDir = CommandLine::absoluteArgPath(take()).string();
        else if (name == "--wait-job") o.waitJob = flag();
        else if (name == "--help") o.help = flag();
        else if (name == "--continue") o.continueOnError = flag();
        else if (name == "--atomic") o.atomic = flag();
        else if (name == "--group") o.group = take();
    }
}

const std::vector<std::string> CALL_OPTIONS = {"--url", "--port", "--token", "--project", "--timeout", "--json", "--structured",
                                                "--quiet", "--dry-run", "--image-dir", "--wait-job", "--help"};

// ---------------------------------------------------------------- tool catalogue

struct Catalog {
    std::vector<Json> tools;   // tools/list entries
    bool live = false;

    const Json* find(const std::string& name) const {
        for (const Json& tool : tools) {
            if (tool.value("name", "") == name) return &tool;
        }
        return nullptr;
    }
    std::vector<std::string> names() const {
        std::vector<std::string> out;
        for (const Json& tool : tools) out.push_back(tool.value("name", ""));
        return out;
    }
};

// The registry the server builds, without an editor (buildToolRegistry needs no App)
Catalog offlineCatalog() {
    mcp::McpRegistry registry;
    mcp::buildToolRegistry(registry);
    Catalog catalog;
    for (const mcp::ToolDef& def : registry.tools()) catalog.tools.push_back(mcp::McpRegistry::toolToJson(def));
    return catalog;
}

std::string groupOf(const Json& tool) {
    if (tool.contains("_meta") && tool["_meta"].is_object()) {
        const std::string group = tool["_meta"].value("jasper/group", std::string());
        if (!group.empty()) return group;
    }
    return mcp::CORE_TOOL_GROUP;
}

size_t toolChars(const Json& tool) {
    const std::string text = tool.value("name", "") + tool.value("description", "") +
                             (tool.contains("inputSchema") ? tool["inputSchema"].dump(-1, ' ', false, Json::error_handler_t::replace) : "{}");
    size_t count = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) count++;
    }
    return count;
}

Json closedSchema(const Json& tool) {
    return tool.contains("inputSchema") ? tool["inputSchema"] : Json{{"type", "object"}};
}

const Json& findToolOrThrow(const Catalog& catalog, const std::string& name) {
    if (const Json* tool = catalog.find(name)) return *tool;
    std::string message = "unknown tool " + name;
    const std::vector<std::string> close = closeMatches(name, catalog.names());
    if (!close.empty()) message += " (did you mean " + close[0] + "?)";
    message += ". `call --help` lists the tools.";
    throw UsageError(message);
}

// ---------------------------------------------------------------- connection

struct Session {
    std::unique_ptr<McpClient> client;
    StdioClient* spawned = nullptr;   // the client, when this call started the editor
    fs::path scratch;
    Json editor;                       // the envelope's editor object
    Catalog catalog;
    bool quiet = false;

    Session() = default;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    ~Session() {
        if (!client) return;
        const long pid = spawned ? spawned->pid() : -1;
        client->close();
        if (spawned && !quiet) std::fprintf(stderr, "[call] the editor (pid %ld) closed; unsaved edits of that session are gone\n", pid);
        if (!scratch.empty()) {
            const char* keep = std::getenv("JASPER_KEEP_SCRATCH");
            if (keep && *keep && std::string(keep) != "0") {
                if (!quiet) std::fprintf(stderr, "[call] scratch kept: %s\n", scratch.string().c_str());
            } else {
                std::error_code ec;
                fs::remove_all(scratch, ec);
            }
        }
    }
};

struct UrlParts {
    int port = 0;
    std::string path = "/mcp";
};

UrlParts parseUrl(const std::string& url) {
    const std::string prefix = "http://";
    if (url.rfind(prefix, 0) != 0) throw UsageError("--url must be http://127.0.0.1:<port>/mcp (got " + url + ")");
    std::string rest = url.substr(prefix.size());
    const size_t slash = rest.find('/');
    std::string hostPort = slash == std::string::npos ? rest : rest.substr(0, slash);
    UrlParts parts;
    if (slash != std::string::npos) parts.path = rest.substr(slash);
    const size_t colon = hostPort.rfind(':');
    const std::string host = colon == std::string::npos ? hostPort : hostPort.substr(0, colon);
    if (host != "127.0.0.1" && host != "localhost") {
        throw UsageError("the editor serves MCP on 127.0.0.1 only; --url host " + host + " is not local");
    }
    if (colon == std::string::npos) throw UsageError("--url needs a port: http://127.0.0.1:<port>/mcp");
    parts.port = std::atoi(hostPort.c_str() + colon + 1);
    if (parts.port < 1 || parts.port > 65535) throw UsageError("--url has no valid port: " + url);
    return parts;
}

std::string settingsToken(const std::string& settingsPath) {
    if (settingsPath.empty()) return {};
    // the editor keeps its token in an owner-only file next to settings.yaml
    // (AppSettings::getMcpHttpTokenPath); older settings held it as mcp.http_token
    {
        std::ifstream in(fs::path(settingsPath).parent_path() / "mcp_http_token", std::ios::binary);
        std::string token;
        if (in && std::getline(in, token)) {
            while (!token.empty() && (token.back() == '\r' || token.back() == ' ' || token.back() == '\t')) token.pop_back();
            if (!token.empty()) return token;
        }
    }
    try {
        const YAML::Node root = YAML::LoadFile(settingsPath);
        if (root["mcp"] && root["mcp"]["http_token"]) return root["mcp"]["http_token"].as<std::string>();
    } catch (const std::exception&) {
    }
    return {};
}

// --token, else JASPER_MCP_TOKEN, else the editor's token file (or the older mcp.http_token)
std::string chooseToken(const Options& o, const InstanceInfo* instance) {
    if (o.tokenSet) return o.token;
    if (const char* env = std::getenv("JASPER_MCP_TOKEN"); env && *env) return env;
    return instance ? settingsToken(instance->settingsPath) : std::string();
}

std::string describeInstance(const InstanceInfo& info) {
    return "  pid " + std::to_string(info.pid) + "  " + info.url + "  " + (info.project.empty() ? "(no project)" : info.project);
}

fs::path executablePath(const char* executableName) {
    fs::path name = fs::path(executableName ? executableName : "jasperengine-editor").filename();
#ifdef _WIN32
    if (!name.has_extension()) name += ".exe";
#endif
    return FileUtils::getExecutableDir() / name;
}

fs::path makeScratchDirectory() {
    std::random_device random;
    std::error_code ec;
    for (int attempt = 0; attempt < 20; attempt++) {
        char suffix[16];
        std::snprintf(suffix, sizeof(suffix), "%08x", static_cast<unsigned>(random()));
        const fs::path dir = fs::temp_directory_path(ec) / ("jasper-call-" + std::string(suffix));
        if (fs::create_directory(dir, ec)) return dir;
    }
    throw ClientError(Connection, "can't create a scratch directory for the editor");
}

double spawnTimeout() {
    const char* value = std::getenv("JASPER_CALL_SPAWN_TIMEOUT");
    const double seconds = value ? std::atof(value) : 0.0;
    return seconds > 0 ? seconds : DEFAULT_SPAWN_TIMEOUT_SECONDS;
}

void attach(Session& session, int port, const std::string& path, const std::string& token, const InstanceInfo* instance) {
    session.client = std::make_unique<HttpClient>(port, path, token);
    session.editor = Json{{"url", "http://127.0.0.1:" + std::to_string(port) + path},
                          {"pid", instance ? Json(instance->pid) : Json(nullptr)},
                          {"project", instance ? Json(instance->project) : Json(nullptr)},
                          {"spawned", false},
                          {"version", instance ? Json(instance->version) : Json(nullptr)}};
}

// the last call's editor was one it started, which stopped or never answered before it had the project
// open (a project another editor holds is not that: it started and refused): CallCommand::spawnFailed
bool lastSpawnFailed = false;

// `mcp --project DIR --headless`, in a scratch working directory with an empty settings.yaml
void spawn(Session& session, const std::string& project, const char* executableName) {
    session.scratch = makeScratchDirectory();
    io::AtomicWriteOptions cache;
    cache.durability = io::Durability::Cache;
    io::writeFileAtomic(session.scratch / "settings.yaml", "", cache);
    const fs::path exe = executablePath(executableName);
    std::unique_ptr<StdioClient> client;
    try {
        client = std::make_unique<StdioClient>(exe, std::vector<std::string>{"mcp", "--project", project, "--headless"}, session.scratch);
    } catch (const ClientError&) {
        lastSpawnFailed = true;
        throw;
    }
    session.spawned = client.get();
    session.editor = Json{{"url", nullptr}, {"pid", client->pid()}, {"project", project}, {"spawned", true}, {"version", nullptr}};
    if (!session.quiet) {
        std::fprintf(stderr, "[call] started an editor (pid %ld) on %s in %s; it closes after this command and does "
                     "not save on its own\n", client->pid(), project.c_str(), session.scratch.string().c_str());
    }
    session.client = std::move(client);
}

// The spawned editor stopped before answering: why, as the exit code says
[[noreturn]] void spawnFailed(Session& session, const ClientError& error) {
    const std::string tail = session.spawned ? session.spawned->stderrTail(40) : std::string();
    const size_t locked = tail.find("project_locked: ");
    if (locked != std::string::npos) {
        const size_t end = tail.find('\n', locked);
        throw Failure{Connection, tail.substr(locked, end == std::string::npos ? std::string::npos : end - locked) +
                                  " (attach to that editor with --port, or close it)"};
    }
    lastSpawnFailed = true;
    throw Failure{error.exitCode, std::string(error.what())};
}

// Resolves the target (S31 R12) and opens the session: --url, --port, JASPER_MCP_URL,
// --project (its running editor, else a one-shot spawn), else the only running editor
void connect(Session& session, const Options& o, const char* executableName) {
    session.quiet = o.quiet;
    const std::vector<InstanceInfo> instances = mcp::liveInstances();
    auto byPort = [&](int port) -> const InstanceInfo* {
        for (const InstanceInfo& info : instances) {
            if (info.port == port) return &info;
        }
        return nullptr;
    };
    if (!o.url.empty() || o.port > 0) {
        const UrlParts parts = !o.url.empty() ? parseUrl(o.url) : UrlParts{o.port, "/mcp"};
        const InstanceInfo* instance = byPort(parts.port);
        attach(session, parts.port, parts.path, chooseToken(o, instance), instance);
    } else if (const char* env = std::getenv("JASPER_MCP_URL"); env && *env) {
        const UrlParts parts = parseUrl(env);
        const InstanceInfo* instance = byPort(parts.port);
        attach(session, parts.port, parts.path, chooseToken(o, instance), instance);
    } else if (!o.project.empty()) {
        const InstanceInfo* match = nullptr;
        for (const InstanceInfo& info : instances) {
            if (mcp::sameProjectPath(info.project, o.project)) match = &info;
        }
        if (match) attach(session, match->port, "/mcp", chooseToken(o, match), match);
        else spawn(session, o.project, executableName);
    } else if (instances.size() == 1) {
        attach(session, instances[0].port, "/mcp", chooseToken(o, &instances[0]), &instances[0]);
    } else if (instances.empty()) {
        throw Failure{Connection, "no running editor serves MCP over HTTP; pass --project <dir> to start one, or "
                                  "--port/--url for an editor started with --mcp-port"};
    } else {
        std::string list;
        for (const InstanceInfo& info : instances) list += "\n" + describeInstance(info);
        throw Failure{Usage, "several editors are running; choose one with --project, --port or --url:" + list};
    }

    session.client->interrupted = []() { return interruptFlag.load(); };
    try {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(static_cast<long long>(spawnTimeout()));
        session.client->initialize(session.spawned ? spawnTimeout() : std::min(o.timeout, 30.0));
        // initialize is answered before the project has loaded: a spawned editor is ready once
        // it has the project open (a locked project makes it exit instead, spawnFailed)
        while (session.spawned) {
            const Json status = session.client->request("tools/call", Json{{"name", "editor_status"}, {"arguments", Json::object()}},
                                                        spawnTimeout());
            const Json path = status.contains("result") ? status["result"].value("structuredContent", Json::object())
                                                              .value("project", Json::object()).value("path", Json()) : Json();
            if (path.is_string() && mcp::sameProjectPath(path.get<std::string>(), o.project)) break;
            if (std::chrono::steady_clock::now() >= deadline) {
                throw ClientError(Connection, "the editor did not open " + o.project + " within " +
                                  std::to_string(static_cast<int>(spawnTimeout())) + " s (JASPER_CALL_SPAWN_TIMEOUT)");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    } catch (const ClientError& error) {
        if (session.spawned) spawnFailed(session, error);
        throw;
    }
    const Json& info = session.client->serverInfo();
    const std::string version = info.value("version", std::string());
    session.editor["version"] = version;
    if (!version.empty() && version != JASPERENGINE_EDITOR_VERSION && !o.quiet) {
        std::fprintf(stderr, "[call] warning: the editor is version %s, this command line %s\n", version.c_str(),
                     JASPERENGINE_EDITOR_VERSION);
    }
    if (!o.quiet && !session.spawned) {
        std::fprintf(stderr, "[call] editor at %s%s\n", session.client->url().c_str(),
                     session.editor["project"].is_string() ? (" (" + session.editor["project"].get<std::string>() + ")").c_str() : "");
    }
    // every group: a group's tools are callable whether the session lists them or not
    const Json listed = session.client->request("tools/list", Json{{"_meta", {{"jasper/groups", "all"}}}}, o.timeout);
    if (listed.contains("error") || !listed["result"].contains("tools") || !listed["result"]["tools"].is_array()) {
        throw ClientError(Internal, "tools/list failed: " + listed.dump().substr(0, 300));
    }
    session.catalog.live = true;
    for (const Json& tool : listed["result"]["tools"]) session.catalog.tools.push_back(tool);
}

// ---------------------------------------------------------------- results

struct CallOutcome {
    int exit = Ok;
    // the call succeeded and the exit code is the tool's own (cli_exit)
    bool toolExit = false;
    std::string tool;
    std::string text;
    Json structured;
    Json images = Json::array();
    std::string error;
};

std::string imageExtension(const std::string& mime) {
    if (mime == "image/jpeg") return "jpg";
    if (mime == "image/webp") return "webp";
    if (mime == "image/gif") return "gif";
    return "png";
}

// <image-dir>/<tool>-<n>.<ext>, never over an existing file
Json saveImage(const Options& o, const std::string& tool, const Json& item) {
    const std::string mime = item.value("mimeType", "image/png");
    const std::vector<unsigned char> bytes = Base64::decode(item.value("data", std::string()));
    const fs::path dir = o.imageDir;
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path path;
    for (int n = 1;; n++) {
        path = dir / (tool + "-" + std::to_string(n) + "." + imageExtension(mime));
        if (!fs::exists(path, ec)) break;
    }
    io::AtomicWriteOptions cache;
    cache.durability = io::Durability::Cache;
    const io::WriteResult written = io::writeFileAtomic(path, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), cache);
    if (!written.ok()) throw Failure{Internal, "can't write the image " + path.string() + ": " + written.message};
    if (!o.quiet) std::fprintf(stderr, "[call] image: %s\n", path.string().c_str());
    return Json{{"path", path.string()}, {"mime_type", mime}, {"bytes", bytes.size()}};
}

// A successful result's own exit code: structuredContent.cli_exit, 1 to 125 (apply_palette
// measure_only's verdict: 3 fixable, 4 needs_person). It shares numbers with the CLI's own codes;
// the --json envelope's code ("tool_exit") tells them apart.
void applyToolExit(CallOutcome& out, const Json& structured) {
    if (out.exit != Ok || !structured.is_object() || !structured.contains("cli_exit") || !structured["cli_exit"].is_number_integer()) return;
    const long long code = structured["cli_exit"].get<long long>();
    if (code < 1 || code > 125) return;
    out.exit = static_cast<int>(code);
    out.toolExit = true;
}

CallOutcome outcomeOf(const Options& o, const std::string& tool, const Json& response) {
    CallOutcome out;
    out.tool = tool;
    if (response.contains("error")) {
        const Json& error = response["error"];
        out.error = error.value("message", error.dump());
        // invalid params (an unknown tool, arguments that are not an object): the caller's to fix
        out.exit = error.value("code", 0) == -32602 ? Usage : Internal;
        out.text = out.error;
        return out;
    }
    const Json& result = response["result"];
    if (!result.is_object()) {
        out.exit = Internal;
        out.error = "tools/call returned no result object";
        return out;
    }
    std::string text;
    if (result.contains("content") && result["content"].is_array()) {
        for (const Json& item : result["content"]) {
            const std::string type = item.value("type", "");
            if (type == "text") text += (text.empty() ? "" : "\n") + item.value("text", std::string());
            else if (type == "image") out.images.push_back(saveImage(o, tool, item));
        }
    }
    out.text = text;
    out.structured = result.contains("structuredContent") ? result["structuredContent"] : Json(nullptr);
    if (result.value("isError", false)) {
        out.exit = ToolError;
        out.error = text;
    }
    applyToolExit(out, out.structured);
    return out;
}

Json envelope(const CallOutcome& out, const Json& editor) {
    const char* code = out.toolExit ? "tool_exit" : codeName(out.exit);
    const bool ok = out.exit == Ok || out.toolExit;
    return Json{
        {"ok", ok}, {"exit_code", out.exit}, {"code", code ? Json(code) : Json(nullptr)},
        {"tool", out.tool}, {"text", out.text}, {"structured", out.structured}, {"images", out.images},
        {"error", out.error.empty() && ok ? Json(nullptr) : Json(out.error.empty() ? out.text : out.error)},
        {"editor", editor.is_null() ? Json(nullptr) : editor}
    };
}

void print(const Options& o, const CallOutcome& out, const Json& editor) {
    if (o.json) {
        std::cout << envelope(out, editor).dump(-1, ' ', false, Json::error_handler_t::replace) << std::endl;
        return;
    }
    std::ostream& stream = out.exit == Ok || out.toolExit ? std::cout : std::cerr;
    if (o.structured && !out.structured.is_null()) {
        stream << out.structured.dump(2, ' ', false, Json::error_handler_t::replace) << std::endl;
    } else if (!out.text.empty()) {
        stream << out.text << std::endl;
    }
}

NotificationSink progressSink(const Options& o) {
    if (o.quiet) return {};
    return [](const Json& notification) {
        if (notification.value("method", "") != "notifications/progress") return;
        const Json& p = notification.contains("params") ? notification["params"] : Json::object();
        std::string line = "[progress]";
        if (p.contains("progress") && p.contains("total") && p["total"].is_number() && p["total"].get<double>() > 0) {
            line += " " + std::to_string(static_cast<int>(100.0 * p["progress"].get<double>() / p["total"].get<double>())) + "%";
        } else if (p.contains("progress")) {
            line += " " + p["progress"].dump();
        }
        if (p.contains("message")) line += " " + p.value("message", std::string());
        std::fprintf(stderr, "%s\n", line.c_str());
    };
}

Json toolCall(Session& session, const Options& o, const std::string& tool, const Json& args) {
    return session.client->request("tools/call", Json{{"name", tool}, {"arguments", args}}, o.timeout, progressSink(o));
}

bool isRunningJob(const Json& structured) {
    return structured.is_object() && structured.contains("job_id") && structured["job_id"].is_string() &&
           structured.value("status", "") == "running";
}

// --wait-job: polls get_job every 500 ms until the job ends; Ctrl-C cancels it (exit 3)
CallOutcome waitForJob(Session& session, const Options& o, CallOutcome started) {
    const std::string jobId = started.structured["job_id"].get<std::string>();
    if (!o.quiet) std::fprintf(stderr, "[call] waiting for job %s\n", jobId.c_str());
    while (true) {
        for (int i = 0; i < 5 && !interruptFlag; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (interruptFlag) {
            interruptFlag = false;
            try {
                toolCall(session, o, "cancel_job", Json{{"job_id", jobId}});
            } catch (const ClientError&) {
            }
            throw Failure{Connection, "interrupted: asked the editor to cancel job " + jobId};
        }
        CallOutcome polled = outcomeOf(o, "get_job", toolCall(session, o, "get_job", Json{{"job_id", jobId}}));
        if (polled.exit != Ok) return polled;
        const std::string status = polled.structured.value("status", "");
        if (status == "running") continue;
        polled.tool = started.tool;
        if (status != "succeeded") {
            polled.exit = ToolError;
            polled.error = "job " + jobId + " " + status + (polled.text.empty() ? "" : ": " + polled.text);
        } else if (polled.structured.contains("result")) {
            applyToolExit(polled, polled.structured["result"]);
        }
        return polled;
    }
}

// ---------------------------------------------------------------- help

std::string sampleValue(const ArgSpec& spec) {
    const Json& s = spec.schema;
    switch (spec.kind) {
        case ArgKind::Enum: return s["enum"][0].is_string() ? s["enum"][0].get<std::string>() : s["enum"][0].dump();
        case ArgKind::Integer: return s.contains("minimum") ? s["minimum"].dump() : "1";
        case ArgKind::Number: return s.contains("minimum") ? s["minimum"].dump() : "1.5";
        case ArgKind::Ref: return "Player";
        case ArgKind::ScalarArray: {
            const size_t n = s.contains("minItems") ? s["minItems"].get<size_t>() : 2;
            const ArgKind item = classify(s.contains("items") ? s["items"] : Json::object());
            std::string out;
            for (size_t k = 0; k < std::max<size_t>(n, 1); k++) {
                out += (k ? "," : "") + std::string(item == ArgKind::String ? "a" : (k == 1 ? "1" : "0"));
            }
            return out;
        }
        case ArgKind::FreeObject: return "key=value";
        case ArgKind::Json: return "'{}'";
        default: return spec.key == "name" ? "Box" : "example";
    }
}

Json sampleJson(const ArgSpec& spec) {
    if (spec.kind == ArgKind::Boolean) return true;
    if (spec.kind == ArgKind::FreeObject) return Json{{"key", "value"}};
    if (spec.kind == ArgKind::Json) return Json::object();
    const std::string text = sampleValue(spec);
    return parseToolArgs(Json{{"type", "object"}, {"properties", {{spec.key, spec.schema}}}},
                         std::vector<std::string>{spec.flag + "=" + text})[spec.key];
}

bool simpleKind(ArgKind kind) {
    return kind != ArgKind::Json && kind != ArgKind::FreeObject;
}

void printToolHelp(const Json& tool, const std::string& exe, bool live) {
    const std::string name = tool.value("name", "");
    std::cout << name;
    if (tool.contains("title")) std::cout << " - " << tool.value("title", "");
    std::cout << (live ? "" : "   (offline: the tool list of this build)") << "\n\n";
    std::cout << tool.value("description", "") << "\n\n";
    const Json annotations = tool.value("annotations", Json::object());
    const Json meta = tool.value("_meta", Json::object());
    auto yesNo = [](bool b) { return b ? "yes" : "no"; };
    std::cout << "Group: " << groupOf(tool) << (groupOf(tool) == mcp::CORE_TOOL_GROUP ? "" : " (callable without enabling it)")
              << ". Read-only: " << yesNo(annotations.value("readOnlyHint", false))
              << ". Destructive: " << yesNo(annotations.value("destructiveHint", false))
              << ". Idempotent: " << yesNo(annotations.value("idempotentHint", false))
              << ". Allowed while playing: " << yesNo(meta.value("jasper/while_playing", true)) << ".\n\n";

    const Json schema = closedSchema(tool);
    const std::vector<ArgSpec> specs = argSpecs(schema);
    if (specs.empty()) {
        std::cout << "No parameters.\n\n";
    } else {
        std::cout << "Parameters:\n";
        for (const ArgSpec& spec : specs) {
            std::string line = "  " + spec.flag + (spec.kind == ArgKind::Boolean ? "[=true|false]" : " <" + describeType(spec.schema) + ">");
            if (spec.required) line += "  (required)";
            if (spec.schema.contains("default")) line += "  default " + spec.schema["default"].dump();
            const std::string description = spec.schema.value("description", std::string());
            if (!description.empty()) line += "\n      " + description;
            if (spec.kind == ArgKind::FreeObject && description.find("{component:") != std::string::npos) {
                line += "\n      Component names: call list_component_types.";
            }
            std::cout << line << "\n";
        }
        std::cout << "\n";
    }

    // two examples: flags (the required parameters, then simple optional ones), and the same as JSON
    std::vector<const ArgSpec*> chosen;
    for (const ArgSpec& spec : specs) {
        if (spec.required) chosen.push_back(&spec);
    }
    for (const ArgSpec& spec : specs) {
        if (chosen.size() >= 2) break;
        if (!spec.required && simpleKind(spec.kind)) chosen.push_back(&spec);
    }
    std::string flags;
    Json json = Json::object();
    for (const ArgSpec* spec : chosen) {
        flags += " " + spec->flag + (spec->kind == ArgKind::Boolean ? std::string() : " " + sampleValue(*spec));
        json[spec->key] = sampleJson(*spec);
    }
    std::cout << "Examples:\n"
              << "  " << exe << " call " << name << flags << "\n"
              << "  " << exe << " call " << name << " '" << json.dump() << "'\n\n"
              << "Options (" << exe << " call --help) go before the tool name. A spawned editor (--project with no\n"
              << "editor open on it) closes after the command without saving: save in the same batch.\n";
}

void printUsage(const std::string& exe, const Catalog& catalog) {
    std::cout
        << "Usage:\n"
        << "  " << exe << " call [options] <tool> [--flag value ... | '{json}' | @file.json | @-]\n"
        << "  " << exe << " call [options] <tool> --help\n"
        << "  " << exe << " batch [options] [--continue] [--atomic] < calls.ndjson\n"
        << "  " << exe << " list-tools [options] [--json] [--group G|all]\n"
        << "  " << exe << " editors [--json]\n\n"
        << "Runs MCP tools of a JasperEngine editor from a shell. Tool names are snake_case or kebab-case;\n"
        << "flags, --help and validation come from each tool's schema.\n\n"
        << "Options (before the tool name):\n"
        << "  --url U          http://127.0.0.1:<port>/mcp of a running editor\n"
        << "  --port N         a running editor's MCP port (editor started with --mcp-port N)\n"
        << "  --token T        bearer token (else JASPER_MCP_TOKEN, else the editor's mcp_http_token file)\n"
        << "  --project DIR    the editor that has DIR open, else a one-shot headless editor on it\n"
        << "                   (it closes after the command and does not save on its own)\n"
        << "  --timeout S      seconds per call (default " << DEFAULT_TIMEOUT_SECONDS << "); then exit 3\n"
        << "  --json           one JSON envelope line per call on stdout\n"
        << "  --structured     print structuredContent instead of the text\n"
        << "  --quiet          no progress or status lines on stderr\n"
        << "  --dry-run        print {tool, arguments} after validation; contacts no editor\n"
        << "  --image-dir D    where images are written (default .): <tool>-<n>.png\n"
        << "  --wait-job       wait for a job the call started (get_job) and print its end\n\n"
        << "Without --url/--port/--project, JASPER_MCP_URL or the only running editor is used.\n"
        << "Exit codes: 0 ok, 1 the tool reported an error, 2 usage, 3 connection or timeout, 4 internal; a successful\n"
        << "result's cli_exit is the exit code instead (apply_palette --measure-only: 3 fixable, 4 needs_person).\n\n";
    std::map<std::string, std::vector<std::string>> groups;
    std::vector<std::string> order;
    for (const Json& tool : catalog.tools) {
        const std::string group = groupOf(tool);
        if (!groups.count(group)) order.push_back(group);
        groups[group].push_back(tool.value("name", ""));
    }
    std::cout << "Tools" << (catalog.live ? "" : " (of this build)") << ":\n";
    for (const std::string& group : order) {
        std::cout << "  " << group << " (" << groups[group].size() << "):";
        size_t column = 100;
        for (const std::string& name : groups[group]) {
            if (column + name.size() > 96) {
                std::cout << "\n     ";
                column = 5;
            }
            std::cout << " " << name;
            column += name.size() + 1;
        }
        std::cout << "\n";
    }
}

// ---------------------------------------------------------------- subcommands

std::vector<std::string> argList(int argc, char** argv) {
    std::vector<std::string> out;
    for (int i = 1; i < argc; i++) out.emplace_back(argv[i]);
    return out;
}

void reportFailure(const Options& o, const std::string& tool, int code, const std::string& message, const Json& editor) {
    if (o.json) {
        CallOutcome out;
        out.exit = code;
        out.tool = tool;
        out.error = message;
        std::cout << envelope(out, editor).dump(-1, ' ', false, Json::error_handler_t::replace) << std::endl;
    } else {
        std::cerr << "error: " << message << std::endl;
    }
}

int runCall(const std::vector<std::string>& args, const char* executableName) {
    const std::string exe = commandName(executableName);
    Options o;
    size_t i = 0;
    std::string tool;
    Json editor = nullptr;
    try {
        parseOptions(args, i, o, CALL_OPTIONS);
        const bool wantsHelp = o.help;
        if (i >= args.size()) {
            Catalog catalog;
            if (wantsHelp && o.hasTarget() && !o.dryRun) {
                Session session;
                connect(session, o, executableName);
                catalog = session.catalog;
            } else {
                catalog = offlineCatalog();
            }
            printUsage(exe, catalog);
            if (!wantsHelp) std::cerr << "error: no tool given" << std::endl;
            return wantsHelp ? Ok : Usage;
        }
        tool = snakeName(args[i++]);
        const std::vector<std::string> tokens(args.begin() + static_cast<long>(i), args.end());
        const bool toolHelp = std::find_if(tokens.begin(), tokens.end(), [](const std::string& t) { return t == "--help" || t == "-h"; }) != tokens.end();
        if (wantsHelp) throw UsageError("--help goes after the tool name for its help: call " + tool + " --help");

        // help and dry runs need no editor: the registry of this build (live when a target is given)
        if (toolHelp || o.dryRun) {
            Catalog catalog;
            std::unique_ptr<Session> session;
            if (toolHelp && o.hasTarget()) {
                session = std::make_unique<Session>();
                connect(*session, o, executableName);
                catalog = session->catalog;
            } else {
                catalog = offlineCatalog();
            }
            const Json& def = findToolOrThrow(catalog, tool);
            if (toolHelp) {
                printToolHelp(def, exe, catalog.live);
                return Ok;
            }
            const Json arguments = parseToolArgs(closedSchema(def), tokens);
            const std::vector<std::string> errors = validate(closedSchema(def), arguments, true);
            if (!errors.empty()) {
                std::string message = "invalid arguments for " + tool + ":";
                for (const std::string& e : errors) message += "\n  " + e;
                throw UsageError(message);
            }
            std::cout << Json{{"tool", tool}, {"arguments", arguments}}.dump() << std::endl;
            return Ok;
        }

        // parse against this build's schema first: a usage error never starts an editor. A tool
        // this build lacks may still exist in an editor named explicitly (another version).
        const Catalog offline = offlineCatalog();
        const Json* known = offline.find(tool);
        if (!known && !o.hasTarget()) findToolOrThrow(offline, tool);
        if (known) parseToolArgs(closedSchema(*known), tokens);

        installInterruptHandler();
        Session session;
        connect(session, o, executableName);
        editor = session.editor;
        const Json& def = findToolOrThrow(session.catalog, tool);
        const Json arguments = parseToolArgs(closedSchema(def), tokens);
        const std::vector<std::string> errors = validate(closedSchema(def), arguments, true);
        if (!errors.empty()) {
            std::string message = "invalid arguments for " + tool + ":";
            for (const std::string& e : errors) message += "\n  " + e;
            throw UsageError(message);
        }
        CallOutcome out = outcomeOf(o, tool, toolCall(session, o, tool, arguments));
        if (o.waitJob && out.exit == Ok && isRunningJob(out.structured)) out = waitForJob(session, o, out);
        print(o, out, session.editor);
        return out.exit;
    } catch (const UsageError& e) {
        reportFailure(o, tool, Usage, e.what(), editor);
        return Usage;
    } catch (const ClientError& e) {
        reportFailure(o, tool, e.exitCode, e.what(), editor);
        return e.exitCode;
    } catch (const Failure& f) {
        reportFailure(o, tool, f.code, f.message, editor);
        return f.code;
    } catch (const std::exception& e) {
        reportFailure(o, tool, Internal, std::string("internal error: ") + e.what(), editor);
        return Internal;
    }
}

struct BatchLine {
    std::string tool;
    Json arguments;
    size_t line = 0;
};

int runBatch(const std::vector<std::string>& args, const char* executableName) {
    Options o;
    o.json = true;   // one envelope line per call, always
    size_t i = 0;
    Json editor = nullptr;
    try {
        std::vector<std::string> allowed = CALL_OPTIONS;
        allowed.push_back("--continue");
        allowed.push_back("--atomic");
        parseOptions(args, i, o, allowed);
        o.json = true;
        if (o.help) {
            std::cout << "Usage: " << commandName(executableName) << " batch [options] [--continue] [--atomic] < calls.ndjson\n\n"
                      << "One {\"tool\": name, \"arguments\": {...}} per line (blank and # lines skipped), run in order over one\n"
                      << "session; one JSON envelope line per call. Stops at the first failure unless --continue.\n"
                      << "--atomic sends the lines as one batch call with undo_group (one undo step; at most 200 lines).\n"
                      << "The exit code is the highest of the calls'. Options as for call (call --help).\n";
            return Ok;
        }
        if (i < args.size()) throw UsageError("unexpected argument " + args[i] + ": batch reads its calls from stdin");

        // every line is checked before anything runs
        std::vector<BatchLine> lines;
        std::string text;
        size_t number = 0;
        while (std::getline(std::cin, text)) {
            number++;
            if (!text.empty() && text.back() == '\r') text.pop_back();
            const size_t start = text.find_first_not_of(" \t");
            if (start == std::string::npos || text[start] == '#') continue;
            const Json parsed = Json::parse(text, nullptr, false);
            if (parsed.is_discarded() || !parsed.is_object() || !parsed.contains("tool") || !parsed["tool"].is_string() ||
                (parsed.contains("arguments") && !parsed["arguments"].is_object() && !parsed["arguments"].is_null())) {
                throw UsageError("line " + std::to_string(number) + ": expected {\"tool\": name, \"arguments\": {...}}");
            }
            BatchLine line;
            line.tool = snakeName(parsed["tool"].get<std::string>());
            line.arguments = parsed.contains("arguments") && parsed["arguments"].is_object() ? parsed["arguments"] : Json::object();
            line.line = number;
            lines.push_back(std::move(line));
        }
        if (lines.empty()) throw UsageError("no calls on stdin");
        if (o.atomic && lines.size() > MAX_ATOMIC_LINES) {
            throw UsageError("--atomic takes at most " + std::to_string(MAX_ATOMIC_LINES) + " lines (one batch call), got " +
                             std::to_string(lines.size()));
        }
        const Catalog offline = offlineCatalog();
        for (const BatchLine& line : lines) {
            if (!o.dryRun) break;
            const Json& def = findToolOrThrow(offline, line.tool);
            const std::vector<std::string> errors = validate(closedSchema(def), line.arguments, true);
            if (!errors.empty()) throw UsageError("line " + std::to_string(line.line) + " (" + line.tool + "): " + errors[0]);
        }
        if (o.dryRun) {
            for (const BatchLine& line : lines) std::cout << Json{{"tool", line.tool}, {"arguments", line.arguments}}.dump() << std::endl;
            return Ok;
        }

        installInterruptHandler();
        Session session;
        connect(session, o, executableName);
        editor = session.editor;
        for (const BatchLine& line : lines) {
            const Json& def = findToolOrThrow(session.catalog, line.tool);
            const std::vector<std::string> errors = validate(closedSchema(def), line.arguments, true);
            if (!errors.empty()) throw UsageError("line " + std::to_string(line.line) + " (" + line.tool + "): " + errors[0]);
        }

        int worst = Ok;
        if (o.atomic) {
            Json calls = Json::array();
            for (const BatchLine& line : lines) calls.push_back(Json{{"tool", line.tool}, {"arguments", line.arguments}});
            CallOutcome whole = outcomeOf(o, "batch", toolCall(session, o, "batch", Json{{"calls", calls}, {"undo_group", true},
                                                                                     {"stop_on_error", !o.continueOnError}}));
            if (whole.exit != Ok || !whole.structured.contains("results")) {
                print(o, whole, session.editor);
                return whole.exit == Ok ? Internal : whole.exit;
            }
            const Json& results = whole.structured["results"];
            for (size_t k = 0; k < results.size(); k++) {
                const Json& item = results[k];
                CallOutcome out;
                out.tool = item.value("tool", std::string());
                out.text = item.value("text", std::string());
                out.structured = item.contains("structured") ? item["structured"] : Json(nullptr);
                out.exit = item.value("ok", false) ? Ok : ToolError;
                if (out.exit != Ok) out.error = out.text;
                if (k + 1 == results.size()) out.images = whole.images;   // the batch result carries them all
                print(o, out, session.editor);
                worst = std::max(worst, out.exit);
            }
            return worst;
        }
        for (const BatchLine& line : lines) {
            CallOutcome out = outcomeOf(o, line.tool, toolCall(session, o, line.tool, line.arguments));
            print(o, out, session.editor);
            worst = std::max(worst, out.exit);
            if (out.exit != Ok && !out.toolExit && !o.continueOnError) break;
        }
        return worst;
    } catch (const UsageError& e) {
        reportFailure(o, "", Usage, e.what(), editor);
        return Usage;
    } catch (const ClientError& e) {
        reportFailure(o, "", e.exitCode, e.what(), editor);
        return e.exitCode;
    } catch (const Failure& f) {
        reportFailure(o, "", f.code, f.message, editor);
        return f.code;
    } catch (const std::exception& e) {
        reportFailure(o, "", Internal, std::string("internal error: ") + e.what(), editor);
        return Internal;
    }
}

int runListTools(const std::vector<std::string>& args, const char* executableName) {
    Options o;
    size_t i = 0;
    try {
        std::vector<std::string> allowed = {"--url", "--port", "--token", "--project", "--timeout", "--json", "--quiet", "--group", "--help"};
        parseOptions(args, i, o, allowed);
        if (o.help) {
            std::cout << "Usage: " << commandName(executableName) << " list-tools [options] [--json] [--group G|all]\n\n"
                      << "Name, group, annotations (r read-only, d destructive, i idempotent) and size in characters of every\n"
                      << "tool; --json prints the tool definitions. Without --url/--port/--project: this build's tools.\n";
            return Ok;
        }
        if (i < args.size()) throw UsageError("unexpected argument " + args[i]);
        Catalog catalog;
        std::unique_ptr<Session> session;
        if (o.hasTarget()) {
            session = std::make_unique<Session>();
            o.quiet = true;
            connect(*session, o, executableName);
            catalog = session->catalog;
        } else {
            catalog = offlineCatalog();
        }
        const std::string group = o.group.empty() ? "all" : o.group;
        if (group != "all" && !mcp::findToolGroup(group)) throw UsageError("unknown tool group " + group);
        Json selected = Json::array();
        for (const Json& tool : catalog.tools) {
            if (group == "all" || groupOf(tool) == group) selected.push_back(tool);
        }
        if (o.json) {
            std::cout << selected.dump(1, ' ', false, Json::error_handler_t::replace) << std::endl;
            return Ok;
        }
        size_t total = 0;
        for (const Json& tool : selected) {
            const Json a = tool.value("annotations", Json::object());
            std::string flags;
            flags += a.value("readOnlyHint", false) ? 'r' : '-';
            flags += a.value("destructiveHint", false) ? 'd' : '-';
            flags += a.value("idempotentHint", false) ? 'i' : '-';
            const size_t chars = toolChars(tool);
            total += chars;
            char line[256];
            std::snprintf(line, sizeof(line), "%-36s %-16s %s %6zu", tool.value("name", "").c_str(), groupOf(tool).c_str(),
                          flags.c_str(), chars);
            std::cout << line << "\n";
        }
        std::cout << selected.size() << " tools, " << total << " characters" << (catalog.live ? "" : " (this build)") << std::endl;
        return Ok;
    } catch (const UsageError& e) {
        std::cerr << "error: " << e.what() << std::endl;
        return Usage;
    } catch (const ClientError& e) {
        std::cerr << "error: " << e.what() << std::endl;
        return e.exitCode;
    } catch (const Failure& f) {
        std::cerr << "error: " << f.message << std::endl;
        return f.code;
    } catch (const std::exception& e) {
        std::cerr << "internal error: " << e.what() << std::endl;
        return Internal;
    }
}

int runEditors(const std::vector<std::string>& args, const char* executableName) {
    Options o;
    size_t i = 0;
    try {
        parseOptions(args, i, o, {"--json", "--help"});
        if (o.help) {
            std::cout << "Usage: " << commandName(executableName) << " editors [--json]\n\n"
                      << "The running editors that serve MCP over HTTP (" << mcp::instancesDirectory().generic_string() << ").\n";
            return Ok;
        }
        if (i < args.size()) throw UsageError("unexpected argument " + args[i]);
        const std::vector<InstanceInfo> instances = mcp::liveInstances();
        if (o.json) {
            Json list = Json::array();
            for (const InstanceInfo& info : instances) {
                list.push_back(Json{{"pid", info.pid}, {"url", info.url}, {"project", info.project}, {"version", info.version},
                                    {"started", info.started}});
            }
            std::cout << list.dump() << std::endl;
            return Ok;
        }
        if (instances.empty()) std::cout << "No running editor serves MCP over HTTP.\n";
        for (const InstanceInfo& info : instances) {
            std::cout << info.pid << "  " << info.url << "  " << (info.project.empty() ? "(no project)" : info.project) << "  "
                      << info.version << "  " << info.started << "\n";
        }
        return Ok;
    } catch (const UsageError& e) {
        std::cerr << "error: " << e.what() << std::endl;
        return Usage;
    }
}

} // namespace

int CallCommand::runCalls(const std::string& project, const std::vector<std::pair<std::string, Json>>& calls, bool continueOnError,
                          std::vector<CallResult>& results, const char* executableName) {
    Options o;
    o.project = project;
    o.quiet = true;
    o.timeout = 3600.0;
    results.clear();
    try {
        Session session;
        connect(session, o, executableName);
        int worst = Ok;
        for (const auto& [tool, arguments] : calls) {
            CallOutcome out = outcomeOf(o, tool, toolCall(session, o, tool, arguments));
            CallResult r;
            r.tool = tool;
            r.exit = out.exit;
            r.ok = out.exit == Ok;
            r.text = out.exit == Ok ? out.text : (out.error.empty() ? out.text : out.error);
            r.structured = out.structured;
            results.push_back(std::move(r));
            worst = std::max(worst, out.exit);
            if (out.exit != Ok && !continueOnError) break;
        }
        return worst;
    } catch (const Failure& f) {
        CallResult r;
        r.exit = f.code;
        r.text = f.message;
        results.push_back(std::move(r));
        return f.code;
    } catch (const std::exception& e) {
        CallResult r;
        r.exit = Internal;
        r.text = std::string("internal error: ") + e.what();
        results.push_back(std::move(r));
        return Internal;
    }
}

bool CallCommand::spawnFailed() {
    return lastSpawnFailed;
}

int CallCommand::run(int argc, char** argv, const char* executableName) {
    lastSpawnFailed = false;
    attachConsole();
    const std::string subcommand = argc > 0 ? argv[0] : "call";
    const std::vector<std::string> args = argList(argc, argv);
    if (subcommand == "batch") return runBatch(args, executableName);
    if (subcommand == "list-tools") return runListTools(args, executableName);
    if (subcommand == "editors") return runEditors(args, executableName);
    return runCall(args, executableName);
}

}
