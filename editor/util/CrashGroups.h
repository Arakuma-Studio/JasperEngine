// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "diag/CrashHandler.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Similar crash, hang and user reports merged into groups (docs/specs/shipped-game-diagnostics.md
// R21, R22, D10), for get_crash_reports group and Window > Analysis > Crash Reports. The frames are
// taken as the report writes them (names where it has them, else module+offset): the editor runs no
// symbolizer, tools/crash/symbolicate.py --group adds names. Both pass the rules of
// tools/crash/fixtures/group/expected.json: change them together.
namespace jasperengine::editor::crashgroups {

    struct Signature {
        // "crash | SIGSEGV | game::Player::update | ...", "hang | lua | Loading level", "user | gameplay"
        std::string text;
        std::string platform;
        std::string buildId;
        // a frame without a name: the signature holds only within one build, so it ends in its build id
        bool unresolved = false;
    };

    struct Group {
        std::string signature;
        std::string kind;
        size_t count = 0;
        std::map<std::string, size_t> versions;     // "unknown" when a report has none
        std::map<std::string, size_t> platforms;
        int64_t firstSeen = 0;                       // Unix seconds, from ReportInfo::timeUnix
        int64_t lastSeen = 0;
        std::vector<std::string> reportIds;          // newest first
        bool unresolved = false;
    };

    // A frame's name: no return type, arguments or template arguments; namespaces, operator names
    // and (anonymous namespace) kept; a lambda as <lambda> after its enclosing function
    std::string shortName(std::string_view symbol);

    // Reads the report's crash.txt, newest hang report or user.txt
    Signature signatureOf(const diag::crash::ReportInfo& report);

    // Most reports first, then the newest last seen, then the signature
    std::vector<Group> group(const std::vector<diag::crash::ReportInfo>& reports);

}
