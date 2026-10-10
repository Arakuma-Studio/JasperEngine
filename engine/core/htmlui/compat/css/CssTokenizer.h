// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_CSS_CSSTOKENIZER_H
#define COMPAT_CSS_CSSTOKENIZER_H

#include "Export.h"
#include "htmlui/compat/css/CssAst.h"

#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::htmlui::compat {

    struct TokenizeOptions {
        // Produce <unicode-range-token>s for "U+..." (the unicode-range descriptor of
        // @font-face; CSS Syntax only makes them there, because "u+a" is also a valid selector)
        bool unicodeRanges = false;
    };

    // A tokenizer parse error that produces no token of its own (a bad string or url is a
    // BadString/BadUrl token instead).
    struct TokenizeError {
        enum class Kind : uint8_t { EofInString, EofInUrl, EofInComment, EofInEscape, InvalidEscape };
        Kind kind;
        uint32_t offset;   // where it happened (with the base offset)
    };

    // CSS Syntax Level 3 tokenization (§4) of UTF-8 text. Offsets are bytes of `css` plus
    // baseOffset (the position of an inline <style>/style="" in its HTML file); "\r\n" is
    // one newline, NUL and invalid UTF-8 read as U+FFFD, as in the spec's preprocessing, but
    // positions stay those of the original bytes. Comments are kept as Comment tokens; the
    // list always ends with one EndOfFile token.
    JASPERENGINE_API std::vector<Token> tokenize(std::string_view css, uint32_t baseOffset = 0,
                                                 const TokenizeOptions& options = {},
                                                 std::vector<TokenizeError>* errors = nullptr);

    // Helpers shared by the parser, the serializer and the rewriters
    JASPERENGINE_API bool isIdentStartCodePoint(uint32_t c);
    JASPERENGINE_API bool isIdentCodePoint(uint32_t c);
    JASPERENGINE_API void appendUtf8(std::string& out, uint32_t codePoint);
    JASPERENGINE_API const char* tokenTypeName(TokenType type);   // "ident", "function", ...

}

#endif //COMPAT_CSS_CSSTOKENIZER_H
