// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// W3: console logs (read_console, clear_console)
#include "McpTools.h"
#include "Project.h"
#include "reload/HotReloadService.h"
#include "util/SourceLocation.h"
#include "PlayViewCommon.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"

#include "Out.h"
#include "AppSettings.h"
#include "EditorHost.h"
#include "mcp/McpServer.h"
#include "util/Scheduler.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <memory>
#include <regex>

namespace jasperengine::editor::mcp {

// ------------------------------------------------------------------ shared helpers

namespace w3 {

namespace {

std::string lowerCopy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool startsWith(const std::string& text, const char* prefix) {
    return text.rfind(prefix, 0) == 0;
}

bool endsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Build output keeps its end (the compiler's summary), other messages their start
std::string clip(const std::string& message, size_t maxChars, bool keepTail) {
    if (maxChars == 0 || message.size() <= maxChars) return message;
    const std::string marker = "\n... [" + std::to_string(message.size() - maxChars) + " chars cut] ...\n";
    if (keepTail) {
        const size_t head = maxChars / 4;
        return message.substr(0, head) + marker + message.substr(message.size() - (maxChars - head));
    }
    return message.substr(0, maxChars) + marker;
}

} // namespace

Json consoleEntryJson(const LogEntry& entry, size_t maxChars) {
    Json json = {
        {"id", entry.id},
        {"type", Out::logTypeName(entry.type)},
        {"level", logLevelName(entry.level)},
        {"category", entry.category},
        {"message", clip(entry.message, maxChars, entry.type == LogType::Build)},
        {"time", entry.timeSeconds},
        {"frame", entry.frame}
    };
    if (!entry.file.empty()) json["source"] = {{"file", entry.file}, {"line", entry.line}};
    // the file locations it names (click-to-source, S23 R27): the logger's source first
    std::vector<SourceLocation> found;
    if (!entry.file.empty() && entry.line > 0) {
        SourceLocation source;
        source.path = entry.file;
        source.line = static_cast<int>(entry.line);
        found.push_back(source);
    }
    for (const SourceLocation& location : findSourceLocations(entry.message)) {
        if (!found.empty() && found[0].path == location.path && found[0].line == location.line) continue;
        found.push_back(location);
    }
    if (!found.empty()) {
        SourceRoots roots;
        if (Project* project = HotReloadService::get().getProject()) {
            roots.project = project->getProjectPath();
            roots.lua = project->getLuaPath();
            roots.assets = project->getAssetsPath();
        }
        Json locations = Json::array();
        for (const SourceLocation& location : found) {
            const ResolvedSource resolved = resolveSourceLocation(location.path, roots);
            Json item = {{"file", resolved.inProject ? resolved.projectPath : location.path}, {"line", location.line},
                         {"in_project", resolved.inProject && resolved.exists}};
            if (location.column > 0) item["column"] = location.column;
            if (resolved.inProject && resolved.projectPath != location.path) item["as_written"] = location.path;
            locations.push_back(std::move(item));
        }
        json["locations"] = std::move(locations);
    }
    return json;
}

bool parseLogType(const std::string& name, LogType& out) {
    const std::string value = lowerCopy(name);
    if (value == "info" || value == "log") out = LogType::Info;
    else if (value == "success") out = LogType::Success;
    else if (value == "warning" || value == "warn") out = LogType::Warning;
    else if (value == "error") out = LogType::Error;
    else if (value == "build") out = LogType::Build;
    else return false;
    return true;
}

bool isNoiseLog(const LogEntry& entry) {
    // Every shader variant the editor compiles logs one of these
    return entry.type == LogType::Build && startsWith(entry.message, "Shader ") &&
           endsWith(entry.message, "generated successfully");
}

bool isProblemLog(const LogEntry& entry) {
    if (isNoiseLog(entry)) return false;
    return entry.type == LogType::Warning || entry.type == LogType::Error || entry.type == LogType::Build;
}

ConsoleResult queryConsole(const ConsoleQuery& query) {
    ConsoleResult result;
    const uint64_t last = Out::lastLogId();
    result.nextCursor = std::max(query.since, last);

    std::regex pattern;
    const bool useRegex = !query.regex.empty();
    if (useRegex) {
        pattern = std::regex(query.regex, std::regex::ECMAScript | std::regex::icase);
    }
    const std::string needle = lowerCopy(query.contains);

    auto matches = [&](const LogEntry& entry) {
        if (query.onlyProblems && !isProblemLog(entry)) return false;
        if (!query.types.empty() &&
            std::find(query.types.begin(), query.types.end(), entry.type) == query.types.end()) {
            return false;
        }
        if (!query.levels.empty() &&
            std::find(query.levels.begin(), query.levels.end(), entry.level) == query.levels.end()) {
            return false;
        }
        if (!query.categories.empty() &&
            std::find(query.categories.begin(), query.categories.end(), entry.category) == query.categories.end()) {
            return false;
        }
        if (query.hasSource >= 0 && (entry.file.empty() ? 0 : 1) != query.hasSource) return false;
        if (!needle.empty() && lowerCopy(entry.message).find(needle) == std::string::npos) return false;
        if (useRegex && !std::regex_search(entry.message, pattern)) return false;
        return true;
    };

    // Everything after the cursor (filters can't be pushed into Out, so read it all; Out
    // keeps a bounded buffer)
    std::vector<LogEntry> logs = Out::getLogsSince(query.since, 0, false);

    std::vector<const LogEntry*> picked;
    for (const LogEntry& entry : logs) {
        if (!query.includeNoise && isNoiseLog(entry)) {
            result.skippedNoise++;
            continue;
        }
        if (matches(entry)) picked.push_back(&entry);
    }

    size_t begin = 0;
    size_t end = picked.size();
    if (query.limit > 0 && picked.size() > query.limit) {
        if (query.tail && query.since == 0) {
            begin = picked.size() - query.limit;
        } else {
            end = query.limit;
            result.more = true;
            // continue right after the last entry returned
            result.nextCursor = picked[end - 1]->id;
        }
    }
    for (size_t i = begin; i < end; i++) {
        const LogEntry& entry = *picked[i];
        if (entry.type == LogType::Error) result.errors++;
        if (entry.type == LogType::Warning) result.warnings++;
        result.entries.push_back(consoleEntryJson(entry, query.maxMessageChars));
    }
    return result;
}

std::string formatConsoleText(const Json& entries, size_t maxChars) {
    // Consecutive repeats of a message collapse into one line ("(x3)")
    std::string text;
    size_t shown = 0;
    size_t i = 0;
    while (i < entries.size()) {
        const Json& entry = entries[i];
        const std::string type = entry.value("type", std::string("info"));
        const std::string message = entry.value("message", std::string());
        size_t repeats = 1;
        while (i + repeats < entries.size() && entries[i + repeats].value("message", std::string()) == message &&
               entries[i + repeats].value("type", std::string()) == type) {
            repeats++;
        }
        std::string line = "[" + type + " #" + std::to_string(entry.value("id", 0ull)) + "] " + message;
        if (repeats > 1) line += " (x" + std::to_string(repeats) + ")";
        if (maxChars > 0 && text.size() + line.size() > maxChars && shown > 0) {
            text += "\n... " + std::to_string(entries.size() - i) + " more (see structured entries)";
            break;
        }
        if (!text.empty()) text += "\n";
        text += line;
        shown++;
        i += repeats;
    }
    return text;
}

} // namespace w3

// ------------------------------------------------------------------ tools

namespace {

// levels / categories of read_console and wait_for_log; false (the call failed) on bad input
bool readLevelAndCategoryFilters(const CallPtr& call, const Json& args, w3::ConsoleQuery& query) {
    if (args.contains("levels") && args["levels"].is_array()) {
        for (const Json& item : args["levels"]) {
            LogLevel level;
            if (!item.is_string() || !parseLogLevel(item.get<std::string>().c_str(), level) || level == LogLevel::Off) {
                call->fail("levels: unknown level " + item.dump() + ". Valid: trace, debug, info, warn, error, fatal.");
                return false;
            }
            query.levels.push_back(level);
        }
    }
    if (args.contains("categories") && args["categories"].is_array()) {
        for (const Json& item : args["categories"]) {
            if (!item.is_string()) {
                call->fail("categories: a list of category names (get_log_config lists them).");
                return false;
            }
            query.categories.push_back(item.get<std::string>());
        }
    }
    return true;
}

void readConsoleTool(const CallPtr& call) {
    const Json& args = call->args();
    w3::ConsoleQuery query;

    if (args.contains("since") && !args["since"].is_null()) {
        const Json& since = args["since"];
        const auto* cursor = since.get_ptr<const std::string*>();
        if (since.is_number_integer() && since.get<long long>() >= 0) {
            query.since = since.get<uint64_t>();
        } else if (cursor && !cursor->empty() &&
                   std::all_of(cursor->begin(), cursor->end(), [](unsigned char c) { return std::isdigit(c); })) {
            query.since = std::stoull(*cursor);
        } else {
            call->fail("since must be a cursor: the next_cursor (a non-negative integer) of an earlier read_console, "
                       "or 0 for everything kept.");
            return;
        }
    } else {
        query.tail = true;
    }

    query.limit = static_cast<size_t>(std::clamp(args.value("limit", 100), 1, 1000));
    query.onlyProblems = args.value("only_problems", false);
    query.includeNoise = args.value("include_shader_builds", false);
    query.maxMessageChars = static_cast<size_t>(std::clamp(args.value("max_message_chars", 4000), 100, 100000));
    query.contains = args.value("contains", "");
    query.regex = args.value("regex", "");

    if (args.contains("types") && args["types"].is_array()) {
        for (const Json& item : args["types"]) {
            LogType type;
            if (!item.is_string() || !w3::parseLogType(item.get<std::string>(), type)) {
                call->fail("types: unknown log type " + item.dump() + ". Valid: info, success, warning, error, build.");
                return;
            }
            query.types.push_back(type);
        }
    }
    if (!readLevelAndCategoryFilters(call, args, query)) return;
    if (args.contains("has_source") && args["has_source"].is_boolean()) {
        query.hasSource = args["has_source"].get<bool>() ? 1 : 0;
    }

    w3::ConsoleResult result;
    try {
        result = w3::queryConsole(query);
    } catch (const std::regex_error& e) {
        call->fail(std::string("regex is not a valid ECMAScript regular expression: ") + e.what());
        return;
    }

    Json data = {
        {"entries", result.entries},
        {"next_cursor", result.nextCursor},
        {"more", result.more},
        {"errors", result.errors},
        {"warnings", result.warnings}
    };
    if (result.skippedNoise > 0) data["skipped_shader_builds"] = result.skippedNoise;

    std::string text;
    if (result.entries.empty()) {
        text = query.tail ? "The console is empty" : "No new console entries";
        if (query.onlyProblems || !query.types.empty() || !query.contains.empty() || !query.regex.empty()) {
            text += " matching the filters";
        }
        text += ".";
    } else {
        text = std::to_string(result.entries.size()) + " entr" + (result.entries.size() == 1 ? "y" : "ies") +
               " (" + std::to_string(result.errors) + " error(s), " + std::to_string(result.warnings) + " warning(s))";
        if (result.more) text += ", more after next_cursor";
        text += ":\n" + w3::formatConsoleText(result.entries);
    }
    text += "\nnext_cursor: " + std::to_string(result.nextCursor);
    call->ok(text, data);
}

void clearConsoleTool(const CallPtr& call) {
    const uint64_t cursor = Out::lastLogId();
    Out::clearConsole();
    call->ok("Cleared the console. Cursors stay valid: next_cursor is " + std::to_string(cursor) + ".",
             Json{{"next_cursor", cursor}});
}

void setLogLevelTool(const CallPtr& call) {
    const Json& args = call->args();
    LogLevel level;
    const std::string levelName = args.value("level", "");
    if (!parseLogLevel(levelName.c_str(), level)) {
        call->fail("level: trace, debug, info, warn, error, fatal or off.");
        return;
    }
    LogCategory* category = nullptr;
    const std::string categoryName = args.value("category", "");
    if (!categoryName.empty()) {
        category = Logger::findCategory(categoryName);
        if (!category) {
            call->fail("category: no category '" + categoryName + "' (get_log_config lists them).");
            return;
        }
    }
    const std::string sink = args.value("sink", "");
    std::string done;
    if (sink.empty()) {
        if (category) {
            Logger::setCategoryLevel(*category, level);
            done = "category '" + categoryName + "' logs at " + logLevelName(level) + " and above";
        } else {
            Logger::setDefaultLevel(level);
            done = "every category logs at " + std::string(logLevelName(level)) + " and above";
        }
    } else if (sink == "mcp" && !category) {
        // the notifications/message of every connected session
        call->server().setAllSessionsLogLevel(level);
        done = "MCP sessions get notifications/message at " + std::string(logLevelName(level)) + " and above";
    } else {
        const Logger::SinkId id = Logger::findSink(sink);
        if (!id) {
            call->fail("sink: no '" + sink + "' sink in this editor (the console sink only exists without a Console window).");
            return;
        }
        Logger::setSinkLevel(id, level, category);
        if (sink == "file" && !category) Out::setFileLevel(level);
        done = "sink '" + sink + "'" + (category ? " takes category '" + categoryName + "'" : std::string(" takes")) +
               " at " + logLevelName(level) + " and above";
    }
    call->ok("Done: " + done + ". Records below a category's own level are never logged anywhere.",
             Json{{"level", logLevelName(level)}, {"category", categoryName}, {"sink", sink}});
}

void getLogConfigTool(const CallPtr& call) {
    Json categories = Json::array();
    for (size_t i = 0; i < Logger::categoryCount(); i++) {
        const LogCategory& category = Logger::categoryAt(i);
        categories.push_back({{"name", category.name},
                              {"level", logLevelName(Logger::categoryLevel(category))},
                              {"effective", logLevelName(static_cast<LogLevel>(category.effective.load()))}});
    }
    Json sinks = Json::array();
    for (const Logger::SinkInfo& info : Logger::sinkInfo()) {
        Json overrides = Json::object();
        for (const auto& [category, level] : info.categoryLevels) overrides[category->name] = logLevelName(level);
        sinks.push_back({{"name", info.name},
                         {"delivery", info.delivery == LogSink::Delivery::Inline ? "inline" : "async"},
                         {"level", logLevelName(info.level)},
                         {"category_levels", overrides}});
    }
    const Logger::Stats stats = Logger::stats();
    Json data = {
        {"categories", categories},
        {"sinks", sinks},
        {"backtrace_on_error", Logger::backtraceOnError()},
        {"command_line_levels", Out::levelOverride()},
        {"counters", {{"records", stats.records}, {"dropped", stats.dropped}, {"suppressed", stats.suppressed},
                      {"backtraces_omitted", stats.backtracesOmitted}}},
        {"queue", {{"async", stats.async}, {"capacity_bytes", stats.queueCapacity}, {"used_bytes", stats.queueUsed},
                   {"high_water_bytes", stats.queueHighWater}}}
    };
    std::string text = std::to_string(categories.size()) + " categories, " + std::to_string(sinks.size()) + " sinks:";
    for (const Json& sink : sinks) {
        text += " " + sink["name"].get<std::string>() + "=" + sink["level"].get<std::string>();
    }
    text += "\ndropped " + std::to_string(stats.dropped) + ", suppressed " + std::to_string(stats.suppressed) + ".";
    call->ok(text, data);
}

void waitForLogTool(const CallPtr& call) {
    const Json& args = call->args();
    auto query = std::make_shared<w3::ConsoleQuery>();
    if (!args.contains("since") || !args["since"].is_number_integer() || args["since"].get<long long>() < 0) {
        call->fail("since: a cursor (read_console's next_cursor, or 0): only entries after it match.");
        return;
    }
    query->since = args["since"].get<uint64_t>();
    query->contains = args.value("contains", "");
    query->regex = args.value("regex", "");
    query->limit = static_cast<size_t>(std::clamp(args.value("limit", 20), 1, 200));
    query->maxMessageChars = 4000;
    query->includeNoise = true;
    if (!readLevelAndCategoryFilters(call, args, *query)) return;
    if (!query->regex.empty()) {
        try {
            std::regex check(query->regex, std::regex::ECMAScript);
        } catch (const std::regex_error& e) {
            call->fail(std::string("regex is not a valid ECMAScript regular expression: ") + e.what());
            return;
        }
    }
    const int timeoutMs = std::clamp(args.value("timeout_ms", 5000), 0, 30000);
    auto found = std::make_shared<w3::ConsoleResult>();
    // checked every editor frame: the store is written when a record is logged (any thread)
    auto check = [query, found]() {
        *found = w3::queryConsole(*query);
        return !found->entries.empty();
    };
    auto finish = [call, found, query](bool timedOut) {
        if (found->entries.empty() || timedOut) {
            const uint64_t cursor = std::max(query->since, Out::lastLogId());
            call->ok("Timed out: nothing matching was logged.", Json{{"timed_out", true}, {"next_cursor", cursor}});
            return;
        }
        Json data = {{"timed_out", false}, {"entries", found->entries}, {"next_cursor", found->nextCursor}};
        call->ok(std::to_string(found->entries.size()) + " matching entr" + (found->entries.size() == 1 ? "y" : "ies") +
                 ":\n" + w3::formatConsoleText(found->entries), data);
    };
    if (check()) {
        finish(false);
        return;
    }
    call->waitUntil(check, timeoutMs, finish);
}

Json timersJson(Scheduler& scheduler, const TimeDomain* domain) {
    const SchedulerStats stats = scheduler.stats();
    Json pending = Json::object();
    Json now = Json::object();
    for (size_t d = 0; d < TIME_DOMAIN_COUNT; d++) {
        if (domain && static_cast<size_t>(*domain) != d) continue;
        pending[timeDomainName(static_cast<TimeDomain>(d))] = stats.pending[d];
        now[timeDomainName(static_cast<TimeDomain>(d))] = stats.now[d];
    }
    Json nearest = Json::array();
    for (const TimerInfo& info : scheduler.pending(domain, 100)) {
        Json item = {{"id", info.id}, {"name", info.debugName}, {"domain", timeDomainName(info.domain)},
                     {"due", info.due}, {"in", info.remaining}};
        if (info.period > 0.0) item["period"] = info.period;
        if (info.remainingRuns > 0) item["runs_left"] = info.remainingRuns;
        if (info.ownerKey) {
            char owner[32];
            std::snprintf(owner, sizeof(owner), "0x%llx", static_cast<unsigned long long>(info.ownerKey));
            item["owner"] = owner;
        }
        nearest.push_back(std::move(item));
    }
    return {{"pending", pending}, {"now", now}, {"posted", stats.posted}, {"ran", stats.ran}, {"nearest", nearest}};
}

void listTimersTool(const CallPtr& call) {
    const Json& args = call->args();
    TimeDomain domain;
    const TimeDomain* filter = nullptr;
    if (args.contains("domain") && args["domain"].is_string()) {
        if (!parseTimeDomain(args["domain"].get<std::string>(), domain)) {
            call->fail("domain: game, unscaled, real, frames or fixed_steps.");
            return;
        }
        filter = &domain;
    }
    Json game = timersJson(Scheduler::game(), filter);
    Json editorTimers = timersJson(scheduler(), filter);
    size_t total = 0;
    for (const auto& [name, count] : game["pending"].items()) total += count.get<size_t>();
    std::string text = std::to_string(total) + " game timer(s) pending";
    for (const auto& [name, count] : game["pending"].items()) {
        if (count.get<size_t>() > 0) text += ", " + name + ": " + std::to_string(count.get<size_t>());
    }
    text += ".";
    call->ok(text, Json{{"game", game}, {"editor", editorTimers}, {"total_game_pending", total}});
}

} // namespace

void registerConsoleTools(McpRegistry& registry) {
    using namespace schema;

    {
        ToolDef tool;
        tool.name = "read_console";
        tool.title = "Read Console";
        tool.description =
            "Read the editor Console (build output, warnings, errors, Lua print/Log, script crashes): entries [{id, "
            "type, level, category, message, time, frame, source?:{file,line}}] oldest first, next_cursor (pass as since for newer ones; omitted: the newest "
            "`limit`), more. play, wait and send_input return their console lines already.";
        tool.inputSchema = object({
            {"since", integer("A previous next_cursor.")},
            {"limit", withDefault(integer("", 1, 1000), 100)},
            {"types", array(stringEnum("", {"info", "success", "warning", "error", "build"}), "")},
            {"levels", array(stringEnum("", {"trace", "debug", "info", "warn", "error", "fatal"}), "")},
            {"categories", array(string(""), "engine, editor, lua, build, render... (get_log_config).")},
            {"has_source", boolean("Only entries with (true) / without (false) a source file:line.")},
            {"only_problems", withDefault(boolean("Warnings, errors, build output."), false)},
            {"contains", string("Substring, case-insensitive.")},
            {"regex", string("ECMAScript, case-insensitive.")},
            {"include_shader_builds", withDefault(boolean("Per-variant shader build lines."), false)},
            {"max_message_chars", withDefault(integer("Cut longer ones (build output keeps its end).", 100, 100000), 4000)}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = readConsoleTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "wait_for_log";
        tool.title = "Wait for Log";
        tool.description =
            "Wait until something matching is logged after `since` (a read_console next_cursor): returns the first "
            "matching entries, or timed_out after timeout_ms.";
        tool.inputSchema = object({
            {"since", integer("Cursor; only later entries match.")},
            {"contains", string("Substring, case-insensitive.")},
            {"regex", string("ECMAScript, case-insensitive.")},
            {"levels", array(stringEnum("", {"trace", "debug", "info", "warn", "error", "fatal"}), "")},
            {"categories", array(string(""), "")},
            {"limit", withDefault(integer("", 1, 200), 20)},
            {"timeout_ms", withDefault(integer("", 0, 30000), 5000)}
        }, {"since"});
        tool.annotations.readOnly = true;
        tool.handler = waitForLogTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "set_log_level";
        tool.title = "Set Log Level";
        tool.description =
            "Set a log level: of a category (or every category), or with sink of one sink (console, file, memory = "
            "the Console, mcp = notifications/message), optionally for one category there. Not saved.";
        tool.inputSchema = object({
            {"level", stringEnum("", {"trace", "debug", "info", "warn", "error", "fatal", "off"})},
            {"category", string("Omitted: every category (or the whole sink).")},
            {"sink", stringEnum("", {"console", "file", "memory", "mcp"})}
        }, {"level"});
        tool.annotations.idempotent = true;
        tool.handler = setLogLevelTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "get_log_config";
        tool.title = "Get Log Config";
        tool.description =
            "The logger: categories with their levels, sinks with theirs, drop and rate-limit counters, queue use.";
        tool.inputSchema = object({});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = getLogConfigTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "list_timers";
        tool.title = "List Timers";
        tool.description =
            "Pending timers (Lua Timer, engine and editor scheduler) per clock, and the 100 nearest with name, owner "
            "and when they are due.";
        tool.inputSchema = object({
            {"domain", stringEnum("", {"game", "unscaled", "real", "frames", "fixed_steps"})}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = listTimersTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "clear_console";
        tool.activity = ToolActivity::Control;  // busy policy (McpServer::busyRefusal)
        tool.title = "Clear Console";
        tool.description =
            "Clear the editor Console. Ids keep counting, so read_console cursors stay valid; returns next_cursor.";
        tool.inputSchema = object({});
        tool.annotations.idempotent = true;
        tool.handler = clearConsoleTool;
        registry.addTool(std::move(tool));
    }
}

}
