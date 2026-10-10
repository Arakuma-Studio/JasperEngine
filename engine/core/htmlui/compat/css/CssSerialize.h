// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_CSS_CSSSERIALIZE_H
#define COMPAT_CSS_CSSSERIALIZE_H

#include "Export.h"
#include "htmlui/compat/css/CssAst.h"

#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::htmlui::compat {

    struct SerializeOptions {
        // Between two tokens that would merge when read again (an ident followed by an ident,
        // a number followed by a dimension, ...): true writes "/**/" as CSSOM does, false a
        // space, which RCSS reads the same in values and selectors.
        bool exactSeparators = false;
        // Runs of whitespace become one space (always; whitespace tokens hold no text).
        // Strings are always written with double quotes: RCSS only understands '"'.
    };

    // Component values -> CSS text (CSS Syntax §9 and CSSOM serialization rules): idents
    // escaped where needed, strings double-quoted, numbers in their shortest decimal form
    // without exponent, independent of the C locale.
    JASPERENGINE_API std::string serialize(const ComponentValue& value, const SerializeOptions& options = {});
    JASPERENGINE_API std::string serialize(const ValueList& values, const SerializeOptions& options = {});
    JASPERENGINE_API std::string serializeToken(const Token& token);

    JASPERENGINE_API std::string serializeIdent(std::string_view ident);     // CSSOM "serialize an identifier"
    JASPERENGINE_API std::string serializeString(std::string_view text);     // "\"...\"" with \" \\ and control escapes
    JASPERENGINE_API std::string serializeUrl(std::string_view url);         // url("...")
    // 0.2, 1, -3.5, 0.333333: at most `decimals` decimals, no exponent, "-0" written as "0"
    JASPERENGINE_API std::string formatNumber(double value, int decimals = 6);

    // The same, appended to `out` (one output buffer instead of a string per token)
    JASPERENGINE_API void appendSerialized(std::string& out, const ComponentValue& value, const SerializeOptions& options = {});
    JASPERENGINE_API void appendSerialized(std::string& out, const ValueList& values, const SerializeOptions& options = {});
    JASPERENGINE_API void appendToken(std::string& out, const Token& token);
    JASPERENGINE_API void appendIdent(std::string& out, std::string_view ident);
    JASPERENGINE_API void appendString(std::string& out, std::string_view text);
    JASPERENGINE_API void appendNumber(std::string& out, double value, int decimals = 6);

}

#endif //COMPAT_CSS_CSSSERIALIZE_H
