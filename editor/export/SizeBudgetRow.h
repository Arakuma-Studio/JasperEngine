// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// One Size Budgets row (docs/specs/build-size-tools.md R12), apart from SizeBudget.h so Project.h
// holds the rows without bringing the MCP JSON types into every source that includes it

#pragma once

#include <string>

namespace jasperengine::editor {

    struct SizeBudgetRow {
        std::string platform = "all";    // all, windows, linux, macos, android, ios, web, desktop
        std::string category = "total";  // total or a Build Report category
        double limitMb = 0.0;            // > 0; 1 MB = 1,048,576 bytes

        bool operator==(const SizeBudgetRow& other) const {
            return platform == other.platform && category == other.category && limitMb == other.limitMb;
        }
        bool operator!=(const SizeBudgetRow& other) const { return !(*this == other); }
    };

}
