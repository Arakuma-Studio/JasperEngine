// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_CSS_RCSSGRAMMAR_H
#define COMPAT_CSS_RCSSGRAMMAR_H

// What RmlUi 6.3's property parsers accept (StyleSheetSpecification.cpp), plus the properties
// the compat runtime registers (§3.11.1): the rewriter checks every value it writes, so RmlUi
// never rejects compat output (the zero-warnings invariant) and a value no rewrite can map is
// reported as HTMLUI-CSS-0004 instead. No RmlUi call: checks run without an initialised
// RmlUi (editor CLI).

#include "Export.h"

#include <string>
#include <string_view>

namespace jasperengine::htmlui::compat {

    class JASPERENGINE_API RcssGrammar {
    public:
        // Is `property` an RCSS property (RmlUi's or compat's registered ones)?
        static bool known(std::string_view property);
        // Would RmlUi parse `value` for `property`? Values with var() are not checked (RmlUi
        // resolves them per element). Unknown properties: false.
        static bool accepts(std::string_view property, std::string_view value);
        // the keywords of a keyword property ("" when the property takes no keywords)
        static std::string keywords(std::string_view property);
    };

}

#endif //COMPAT_CSS_RCSSGRAMMAR_H
