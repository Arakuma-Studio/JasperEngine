// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The one result shape of every pixel-art lint (S46's craft lints, S48's effect and rule lints,
// S49's particle lints): {id, status, measured, threshold, threshold_source, count, at, message,
// fix}. Producers fill LintResult; toJson gives the MCP and CLI shape. Pure data, any thread.
// docs/specs/S44-aseprite-import.md §6.3, S46 R25.

#pragma once

#include "json.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::editor::pixelart::lint {

    enum class LintStatus : uint8_t { Pass, Warn, Fail, Skip };

    // "PASS", "WARN", "FAIL", "SKIP"
    const char* lintStatusName(LintStatus status);
    bool parseLintStatus(std::string_view text, LintStatus& out);

    // A pixel a lint points at: frames from 1 (as Aseprite numbers them), x/y from the top-left, from 0
    struct LintSpot {
        int frame = 0, x = 0, y = 0;
        bool operator==(const LintSpot& other) const { return frame == other.frame && x == other.x && y == other.y; }
    };

    struct LintResult {
        // at most this many spots go in `at`; `count` counts them all
        static constexpr size_t kMaxSpots = 8;

        std::string id;                       // snake_case: "off_palette", "orphans", ...
        LintStatus status = LintStatus::Skip;
        double measured = 0, threshold = 0;
        std::string thresholdSource;          // "default", "rules.max_colors", "options.<key>", a project setting
        int count = 0;
        std::vector<LintSpot> at;
        std::string message, fix;             // a SKIP's message says why it was skipped

        // Counts a spot and keeps it while fewer than kMaxSpots are kept
        void addSpot(const LintSpot& spot);
    };

    struct LintReport {
        std::vector<LintResult> results;
        int pass = 0, warn = 0, fail = 0, skip = 0;

        // Appends a result and counts its status
        void add(LintResult result);
        // The status counts from `results` again (after results were edited in place)
        void recount();
        // FAIL if any result failed, else WARN if any warned, else PASS (all skipped: PASS)
        LintStatus overall() const;
    };

    // {"id", "status", "measured", "threshold", "threshold_source", "count", "at": [[frame, x, y], ...],
    //  "message", "fix"}; "message" and "fix" are left out when empty
    nlohmann::json toJson(const LintResult& result);
    // {"status": overall, "pass", "warn", "fail", "skip", "results": [...]}
    nlohmann::json toJson(const LintReport& report);

}
