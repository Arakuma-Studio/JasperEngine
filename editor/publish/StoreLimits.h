// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The stores' limits on what a build writes (docs/specs/build-profiles.md R16): one table, checked
// at build (warnings) and by publish prepare (errors) for a target whose Build Profile names a
// store. Characters are counted as Unicode code points.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::editor::publish {

    struct StoreLimitInput {
        std::string store;        // google-play app-store steam itch web none ("" none)
        std::string storeTitle;
        std::string label;        // the resolved home-screen label
        std::string identifier;   // the resolved application id
        std::string version;      // the resolved version shown in the store
    };

    struct StoreFinding {
        std::string code;         // W-STORE-TITLE-LENGTH, W-LABEL-TRUNCATED, E-STORE-IDENTIFIER, E-STORE-VERSION
        std::string message;
        bool error = false;       // an error at publish prepare (a warning at build either way)
    };

    struct StoreLimitRow {
        const char* store;
        const char* checked;      // "store_title", "label"
        size_t limit;
        const char* code;
        const char* source;       // where the number comes from
    };

    // The table's length rows (StoreLimitsTest pins them)
    const std::vector<StoreLimitRow>& storeLimitRows();
    bool isStoreName(const std::string& name);
    size_t codePoints(std::string_view utf8);
    std::vector<StoreFinding> checkStoreLimits(const StoreLimitInput& input);

}
