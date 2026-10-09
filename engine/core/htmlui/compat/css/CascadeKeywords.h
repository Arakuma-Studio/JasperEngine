// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_CSS_CASCADEKEYWORDS_H
#define COMPAT_CSS_CASCADEKEYWORDS_H

// inherit, initial, unset, revert, revert-layer (phase-2 §3.9.6 step 5) until the fork has
// CssWideKeywords. Inherited properties: `inherit`/`unset` -> var(--html-unset), the never
// defined variable that makes RmlUi treat the property as unset (it inherits the parent's
// computed value, exact). Non-inherited ones: `unset`/`initial` -> var(--html-unset) where
// RmlUi's default is the CSS initial value, else that value; `inherit` -> the textual
// companion var(--html-inh-P, <initial>).

#include "htmlui/compat/css/CssAst.h"
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::htmlui::compat {

    struct DeclContext;

    class CascadeKeywords {
    public:
        // the CSS-wide keyword a value is ("" when it is not one)
        static std::string keywordOf(const ValueList& value);
        // the same for serialized (output) text
        static bool isKeywordText(std::string_view value);
        // writes the declaration for a CSS-wide keyword
        static void apply(DeclContext& context, const std::string& keyword);
        // the RCSS properties a CSS property writes (shorthands expanded)
        static std::vector<std::string> outputProperties(const std::string& cssProperty);
        // is an RCSS property inherited (CSS inheritance)?
        static bool inherited(const std::string& rcssProperty);
    };

}

#endif //COMPAT_CSS_CASCADEKEYWORDS_H
