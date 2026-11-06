// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "util/BuildDiagnostics.h"

#include "util/SourceLocation.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <regex>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    bool contains(const std::string& text, const char* needle) {
        return text.find(needle) != std::string::npos;
    }

    // MSBuild repeats every error at the end of the build with " [<project>.vcxproj]"
    std::string withoutMsBuildProject(std::string line) {
        const size_t open = line.rfind(" [");
        if (open != std::string::npos && !line.empty() && line.back() == ']' && line.find(".vcxproj", open) != std::string::npos) {
            line.erase(open);
        }
        const size_t first = line.find_first_not_of(" \t");
        return first == std::string::npos ? std::string() : line.substr(first);
    }

}

std::string BuildDiagnostic::summary() const {
    if (file.empty()) return message.empty() ? text : message;
    return file + ":" + std::to_string(line) + ": " + message;
}

std::string BuildDiagnostics::stripAnsi(const std::string& line) {
    std::string out;
    out.reserve(line.size());
    for (size_t i = 0; i < line.size(); i++) {
        if (line[i] == '\x1b' && i + 1 < line.size() && line[i + 1] == '[') {
            i += 2;
            while (i < line.size() && !(line[i] >= '@' && line[i] <= '~')) i++;
            continue;
        }
        out += line[i];
    }
    return out;
}

LogLevel BuildDiagnostics::lineLevel(const std::string& line) {
    // GCC/Clang, MSVC, MSBuild, CMake and the linkers
    if (contains(line, ": error") || contains(line, ": fatal error") || contains(line, " error C") ||
        contains(line, "error LNK") || contains(line, ": error MSB") || contains(line, "CMake Error") ||
        contains(line, "undefined reference") || contains(line, "collect2: error") || contains(line, "LINK : fatal error")) {
        return LogLevel::Error;
    }
    if (contains(line, ": warning") || contains(line, " warning C") || contains(line, "CMake Warning")) {
        return LogLevel::Warn;
    }
    return LogLevel::Info;
}

bool BuildDiagnostics::parse(const std::string& rawLine, BuildDiagnostic& out) {
    const std::string line = withoutMsBuildProject(stripAnsi(rawLine));
    if (line.find("error") == std::string::npos && line.find("warning") == std::string::npos &&
        line.find("Error") == std::string::npos && line.find("Warning") == std::string::npos &&
        line.find("undefined reference") == std::string::npos) {
        return false;
    }
    const size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos) return false;
    out = BuildDiagnostic();
    out.text = line;
    const std::vector<SourceLocation> locations = findSourceLocations(line);

    // CMake Error at file:line (command)
    static const std::string cmakeError = "CMake Error", cmakeWarning = "CMake Warning";
    if (line.compare(first, cmakeError.size(), cmakeError) == 0 || line.compare(first, cmakeWarning.size(), cmakeWarning) == 0) {
        const bool warning = line.compare(first, cmakeWarning.size(), cmakeWarning) == 0;
        const size_t at = line.find(" at ", first);
        out.severity = warning ? "warning" : "error";
        out.code = "cmake";
        if (at == std::string::npos || locations.empty() || locations[0].begin != at + 4) {
            // "CMake Error: <message>" without a location
            if (warning) return false;
            out.message = line.substr(first);
            return true;
        }
        out.file = locations[0].path;
        out.line = locations[0].line;
        out.message = std::string(warning ? "CMake Warning" : "CMake Error") + line.substr(locations[0].end);
        return true;
    }

    // a compiler line starts with its location, then ": error: ..." or ": error C1234: ..."
    if (!locations.empty() && locations[0].begin == first) {
        const SourceLocation& location = locations[0];
        size_t at = location.end;
        while (at < line.size() && (line[at] == ' ' || line[at] == ':')) at++;
        std::string severity;
        for (const char* word : {"fatal error", "error", "warning"}) {
            if (line.compare(at, std::strlen(word), word) == 0) {
                severity = word;
                at += std::strlen(word);
                break;
            }
        }
        if (severity.empty()) return false;
        // MSVC's code: error C2065: ...
        std::string code;
        size_t codeEnd = at;
        while (codeEnd < line.size() && line[codeEnd] == ' ') codeEnd++;
        size_t letters = codeEnd;
        while (letters < line.size() && std::isalpha(static_cast<unsigned char>(line[letters]))) letters++;
        size_t digits = letters;
        while (digits < line.size() && std::isdigit(static_cast<unsigned char>(line[digits]))) digits++;
        if (letters > codeEnd && digits > letters) {
            size_t colon = digits;
            while (colon < line.size() && line[colon] == ' ') colon++;
            if (colon < line.size() && line[colon] == ':') {
                code = line.substr(codeEnd, digits - codeEnd);
                at = colon;
            }
        }
        if (at >= line.size() || line[at] != ':') return false;
        at++;
        while (at < line.size() && line[at] == ' ') at++;
        out.file = location.path;
        out.line = location.line;
        out.column = location.column;
        out.severity = severity == "warning" ? "warning" : "error";
        out.code = code;
        out.message = line.substr(at);
        return true;
    }

    static const std::regex linker(R"((undefined reference to .*|multiple definition of .*|ld(?:\.\w+)?: (?:error|fatal error): .*|collect2: error: .*|LINK : fatal error .*|error LNK\d+: .*|error MSB\d+: .*))");
    std::smatch m;
    if (std::regex_search(line, m, linker)) {
        out.severity = "error";
        out.code = line.find("MSB") != std::string::npos && line.find("error MSB") != std::string::npos ? "msbuild" : "link";
        out.message = line.substr(first);
        return true;
    }
    return false;
}

void BuildErrorLog::add(const std::string& line) {
    const std::string clean = BuildDiagnostics::stripAnsi(line);
    if (clean.find_first_not_of(" \t\r\n") != std::string::npos) last = clean;
    BuildDiagnostic diagnostic;
    if (!BuildDiagnostics::parse(line, diagnostic) || diagnostic.severity != "error") return;
    // MSBuild repeats every error at its end, past the kept ones too
    if (!seen.insert(diagnostic.text).second) return;
    count++;
    if (kept.size() < kMaxKept) kept.push_back(std::move(diagnostic));
}

void BuildErrorLog::clear() {
    kept.clear();
    seen.clear();
    count = 0;
    last.clear();
}

std::string BuildErrorLog::failureDetail() const {
    if (kept.empty()) {
        return last.empty() ? std::string(".") : "; the last output was: " + last;
    }
    return " with " + std::to_string(count) + " error(s). First: " + kept.front().summary();
}
