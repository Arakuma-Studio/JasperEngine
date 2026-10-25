// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// References inside source text: HTML `href`/`src`, CSS `@import` and `url()`, Lua
// `require`, shader `#include`. The dependency index keeps them as path edges; renaming
// their target does not rewrite them (S04 R14), the editor lists the ones that break.

#pragma once

#include "asset/DepEdge.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor {

    struct TextReference {
        std::string target;     // as written (without query or fragment)
        uint32_t line = 1;      // 1-based
        DepEdgeKind kind = DepEdgeKind::Other;
    };

    class TextReferences {
    public:
        static std::vector<TextReference> scanHtml(const std::string& text);
        static std::vector<TextReference> scanCss(const std::string& text);
        static std::vector<TextReference> scanLua(const std::string& text);
        static std::vector<TextReference> scanShader(const std::string& text);

        // A target that names a file of the project: not a URL, data URI, anchor or absolute path
        static bool isLocalTarget(const std::string& target);
        // "a.b.c" -> "a/b/c.lua"
        static std::string luaModulePath(const std::string& module);
    };

}
