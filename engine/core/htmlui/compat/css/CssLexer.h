// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_CSS_CSSLEXER_H
#define COMPAT_CSS_CSSLEXER_H

// Internal to the compat layer (CssTokenizer.cpp, CssParser.cpp): the tokenizer's output in
// compact form. A LexToken holds no string: its value and unit are views (offset + length)
// into the source text when they are written there as they are (the common case: names
// without escapes, strings without escapes), or into LexResult::text when they had to be
// decoded. tokenize() turns them into Tokens; the parser builds the Token of each component
// value straight from them, so no intermediate Token (two std::strings) is made per token.

#include "htmlui/compat/css/CssAst.h"
#include "htmlui/compat/css/CssTokenizer.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::htmlui::compat {

    // a run of text: in the source (decoded == false) or in LexResult::text
    struct LexText {
        uint32_t begin = 0;
        uint32_t length = 0;
        bool decoded = false;
    };

    struct LexToken {
        TokenType type = TokenType::EndOfFile;
        bool isInteger = false;
        bool hashIsId = false;
        uint32_t offset = 0, length = 0;          // as Token (with the base offset)
        LexText value, unit;
        double number = 0;
        uint32_t rangeStart = 0, rangeEnd = 0;
    };

    struct LexResult {
        std::string_view source;                   // the tokenized text (without the base offset)
        std::string text;                          // decoded values
        std::vector<LexToken> tokens;              // ends with one EndOfFile token

        std::string_view view(const LexText& t) const {
            return std::string_view((t.decoded ? text.data() : source.data()) + t.begin, t.length);
        }
        std::string_view value(const LexToken& token) const { return view(token.value); }
        // the Token tokenize() returns for it
        void toToken(const LexToken& lex, Token& out) const {
            out.type = lex.type;
            out.isInteger = lex.isInteger;
            out.hashIsId = lex.hashIsId;
            out.offset = lex.offset;
            out.length = lex.length;
            out.rangeStart = lex.rangeStart;
            out.rangeEnd = lex.rangeEnd;
            out.number = lex.number;
            // (most tokens have neither: whitespace, punctuation, numbers)
            if (lex.value.length > 0){
                const std::string_view v = view(lex.value);
                out.value.assign(v.data(), v.size());
            }else{
                out.value.clear();
            }
            if (lex.unit.length > 0){
                const std::string_view u = view(lex.unit);
                out.unit.assign(u.data(), u.size());
            }else{
                out.unit.clear();
            }
        }
    };

    // tokenize() without the Token conversion; comments false: no Comment tokens (the parser
    // drops them)
    void lexCss(std::string_view css, uint32_t baseOffset, const TokenizeOptions& options, std::vector<TokenizeError>* errors,
                LexResult& out, bool comments = true);

}

#endif //COMPAT_CSS_CSSLEXER_H
