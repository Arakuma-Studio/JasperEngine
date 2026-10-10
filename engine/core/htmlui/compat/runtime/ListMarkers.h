// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_RUNTIME_LISTMARKERS_H
#define COMPAT_RUNTIME_LISTMARKERS_H

#include "Export.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace jasperengine::htmlui::compat {

    // The text of a list marker (phase-2 §3.11.6), as Chromium formats it: counter styles get
    // the suffix ". ", bullets and disclosure triangles a space, a string is used as written.
    class JASPERENGINE_API ListMarkers {
    public:
        enum class Type : uint8_t {
            None, Disc, Circle, Square, Decimal, DecimalLeadingZero, LowerAlpha, UpperAlpha,
            LowerRoman, UpperRoman, LowerGreek, DisclosureOpen, DisclosureClosed, String
        };

        // A list-style-type keyword (case-insensitive; lower-latin/upper-latin are the alpha
        // types). The other CSS predefined counter styles (armenian, cjk-decimal, ...) count
        // in decimal, their CSS fallback; anything else is a string (RmlUi's string parser
        // gives author strings without quotes), Type::String.
        static Type parseType(std::string_view keyword);
        static const char* typeName(Type type);           // "lower-roman"; "string" for String
        static bool isCounter(Type type);                  // numbered (decimal ... lower-greek)

        // The marker for the counter value `index`. Alphabetic and roman styles fall back to
        // decimal outside their range (alpha/greek: index < 1; roman: 1-3999), as CSS counter
        // styles do. Type::None gives "".
        static std::string text(Type type, int index);
        // the same for a list-style-type value: a keyword, or a string (quotes removed)
        static std::string text(std::string_view listStyleType, int index);

        // the bare counter representation without the suffix ("iv", "01", "-3")
        static std::string counter(Type type, int index);
    };

}

#endif //COMPAT_RUNTIME_LISTMARKERS_H
