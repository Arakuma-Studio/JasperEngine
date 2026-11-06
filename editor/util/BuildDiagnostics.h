// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Compiler, linker, MSBuild and CMake problem lines of a build's output: what the Console colours,
// what an export's failure message and Build dialog list, what MCP's build tools report. One
// matcher and one parser for all of them.

#include "log/LogCategory.h"

#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor {

    struct BuildDiagnostic {
        std::string file;        // as the line names it ("" for a linker or MSBuild line)
        int line = 0;
        int column = 0;
        std::string severity;    // error, warning
        std::string code;        // C2065, cmake, link, ...
        std::string message;
        std::string text;        // the whole line, ANSI colours removed

        // "file:line: message" (or the line itself without a location)
        std::string summary() const;
    };

    class BuildDiagnostics {
    public:
        static std::string stripAnsi(const std::string& line);
        // the problem level of a build output line (Error, Warn, else Info)
        static LogLevel lineLevel(const std::string& line);
        // file:line:col: error: message (GCC/Clang), file(line,col): error C1234: message (MSVC),
        // CMake Error at file:line, linker and MSBuild errors
        static bool parse(const std::string& line, BuildDiagnostic& out);
    };

    // The errors of one build run: the first kMaxKept unique ones (MSBuild's repeats with a
    // trailing " [x.vcxproj]" collapse into the first), all of them counted, and the last output line
    class BuildErrorLog {
    public:
        static constexpr size_t kMaxKept = 20;

        void add(const std::string& line);
        void clear();
        size_t errorCount() const { return count; }
        const std::vector<BuildDiagnostic>& errors() const { return kept; }
        const std::string& lastLine() const { return last; }
        // what a failure message says after "Build failed": " with N error(s). First: <summary>",
        // or "; the last output was: <line>" when no error line was recognised
        std::string failureDetail() const;

    private:
        std::vector<BuildDiagnostic> kept;
        std::set<std::string> seen;
        size_t count = 0;
        std::string last;
    };

}
