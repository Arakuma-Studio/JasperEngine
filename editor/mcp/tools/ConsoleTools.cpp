// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// W3: console logs (read_console, clear_console)
#include "McpTools.h"
#include "PlayViewCommon.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"

#include "Out.h"

#include <algorithm>
#include <cctype>
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

Json entryToJson(const LogEntry& entry, size_t maxChars) {
    return {
        {"id", entry.id},
        {"type", Out::logTypeName(entry.type)},
        {"message", clip(entry.message, maxChars, entry.type == LogType::Build)},
        {"time", entry.timeSeconds}
    };
}

} // namespace

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
        if (!query.includeNoise && isNoiseLog(entry)) return false;
        if (query.onlyProblems && !isProblemLog(entry)) return false;
        if (!query.types.empty() &&
            std::find(query.types.begin(), query.types.end(), entry.type) == query.types.end()) {
            return false;
        }
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
        result.entries.push_back(entryToJson(entry, query.maxMessageChars));
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

void readConsoleTool(const CallPtr& call) {
    const Json& args = call->args();
    w3::ConsoleQuery query;

    if (args.contains("since") && !args["since"].is_null()) {
        const Json& since = args["since"];
        if (since.is_number_integer() && since.get<long long>() >= 0) {
            query.since = since.get<uint64_t>();
        } else if (since.is_string() && !since.get<std::string>().empty() &&
                   std::all_of(since.get<std::string>().begin(), since.get<std::string>().end(), ::isdigit)) {
            query.since = std::stoull(since.get<std::string>());
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

} // namespace

void registerConsoleTools(McpRegistry& registry) {
    using namespace schema;

    {
        ToolDef tool;
        tool.name = "read_console";
        tool.title = "Read Console";
        tool.description =
            "Read the editor Console (build output, warnings, errors, Lua print/Log, script crashes). Returns "
            "{entries: [{id, type, message, time}] oldest first, next_cursor, more}; pass next_cursor as since "
            "to get only newer entries (omitted: the newest `limit`). play, wait and send_input already return "
            "their console lines.";
        tool.inputSchema = object({
            {"since", integer("A previous next_cursor.")},
            {"limit", withDefault(integer("", 1, 1000), 100)},
            {"types", array(stringEnum("", {"info", "success", "warning", "error", "build"}), "")},
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
