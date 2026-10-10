// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_CSS_IMPORT_H
#define COMPAT_CSS_IMPORT_H

// @import (phase-2 §3.9.2): `@import url(x) [layer(name)] [supports(cond)] [media-list];`
// replaced in place by the imported sheet's rules, wrapped in the corresponding conditions.
// Relative URLs resolve against the importing file, remote ones through the import manifest;
// cycles (HTMLUI-AT-0007 Error) and depth > 16 are cut.

#include "htmlui/compat/css/CssAst.h"
#include "htmlui/compat/css/MediaQuery.h"

#include <optional>
#include <string>
#include <vector>

namespace jasperengine::htmlui::compat {

    struct Session;

    struct ImportedSheet {
        uint32_t file = 0;                 // SourceFiles id of the imported sheet
        Stylesheet sheet;
        MediaResult media = MediaResult::unconditional();
        bool layered = false;              // layer or layer(name)
        std::string layer;                 // "" for an anonymous layer
        ValueList supports;   // the supports() condition, empty when none
    };

    class Import {
    public:
        static constexpr int kMaxDepth = 16;

        // Reads and parses the sheet an @import rule names. `chain`: the files of the current
        // import chain (cycle detection). nullopt when nothing is imported (reported).
        static std::optional<ImportedSheet> load(Session& session, const Rule& rule, uint32_t fromFile,
                                                 const std::vector<uint32_t>& chain);
    };

}

#endif //COMPAT_CSS_IMPORT_H
