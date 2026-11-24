// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Size budgets (docs/specs/build-size-tools.md R12-R14): Project Settings > Player > Publishing
// Settings > Size Budgets, rows of {platform, category, limit in MB}, checked at export against the
// Build Report's categories. A row for the build's platform replaces a desktop row, which replaces
// an all row, of the same category. Source exports are never checked.

#pragma once

#include "export/SizeBudgetRow.h"
#include "mcp/McpJson.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor {

    struct BuildReportBudget;
    class BuildReport;

    namespace sizebudget {
        const std::vector<std::string>& platformNames();
        const std::vector<std::string>& categoryNames();
        // the categories known before the engine is measured (checked before any compile)
        bool isContentCategory(const std::string& category);
        uint64_t limitBytes(double limitMb);

        mcp::Json toJson(const std::vector<SizeBudgetRow>& rows);
        // false with the fix for a value that is not a list of {platform, category, limit_mb}
        bool fromJson(const mcp::Json& value, std::vector<SizeBudgetRow>& rows, std::string& error);

        // the rows that apply to a build of `platform`, one per category, in category order
        std::vector<SizeBudgetRow> applicable(const std::vector<SizeBudgetRow>& rows, const std::string& platform);
        // Checks the applicable rows of `categories` (content: the content categories; else engine
        // and total) against the report's numbers. No rows, or a source export: nothing evaluated.
        std::vector<BuildReportBudget> evaluate(const std::vector<SizeBudgetRow>& rows, const BuildReport& report, bool content);
        // how many evaluations ran (the tests read that none did without rows)
        uint64_t evaluations();
        // W-SIZE-BUDGET's text: platform, category, size, limit, excess, the category's three
        // largest files and the fix
        std::string overText(const BuildReportBudget& budget, const BuildReport& report);
    }

}
