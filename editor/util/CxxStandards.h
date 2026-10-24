// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <iterator>
#include <string>

namespace jasperengine::editor {

    // JASPERENGINE_CXX_STANDARDS of engine/CMakeLists.txt, which passes it to the editor
    // targets; the first entry is the default
    inline constexpr int cxxStandards[] = { JASPERENGINE_CXX_STANDARD_LIST };

    inline bool isSupportedCxxStandard(int standard) {
        for (int supported : cxxStandards) {
            if (supported == standard) return true;
        }
        return false;
    }

    inline int sanitizeCxxStandard(int standard) {
        return isSupportedCxxStandard(standard) ? standard : cxxStandards[0];
    }

    // "20 or 23", or "20, 23, or 26" from three entries on, for settings text, AI tool
    // schemas and generated docs
    inline std::string cxxStandardList() {
        const size_t count = std::size(cxxStandards);
        std::string list;
        for (size_t i = 0; i < count; i++) {
            if (i > 0) list += (i + 1 < count) ? ", " : (count == 2) ? " or " : ", or ";
            list += std::to_string(cxxStandards[i]);
        }
        return list;
    }

}
