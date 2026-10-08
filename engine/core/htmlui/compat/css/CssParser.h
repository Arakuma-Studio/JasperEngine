// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_CSS_CSSPARSER_H
#define COMPAT_CSS_CSSPARSER_H

#include "Export.h"
#include "htmlui/compat/css/CssAst.h"
#include "htmlui/compat/css/CssTokenizer.h"

#include <optional>
#include <string_view>
#include <vector>

namespace jasperengine::htmlui::compat {

    class DiagnosticSink;

    // A syntax error the parser recovered from (CSS Syntax §5), at [offset, offset + length).
    struct ParseError {
        enum class Kind : uint8_t {
            InvalidRule,          // a qualified rule ended at EOF or at ';' in a block, or "--x:..{}"
            InvalidDeclaration,   // something in a block that is neither a declaration nor a rule
            UnclosedBlock,        // {, [ or ( not closed before the end of the input
            UnclosedFunction,     // f( not closed before the end of the input
            UnmatchedCloser,      // a stray ')' or ']' among component values
            BadString,            // a newline inside a string
            BadUrl,               // an invalid unquoted url(...)
            EofInString,
            EofInUrl,
            EofInComment,
            InvalidEscape,
            Empty,                // "parse a rule/declaration/component value" of an empty input
            ExtraInput,           // ... with more than one item
        };
        Kind kind;
        uint32_t offset = 0;
        uint32_t length = 0;
    };

    // ---- what the compatibility layer uses (CSS Syntax 2024: nesting, mixed blocks)

    // A whole stylesheet: rules with their blocks parsed as mixed declarations and nested
    // rules. Syntax errors are recovered as browsers do and reported as HTMLUI-CSS-0008.
    // baseOffset: where `css` starts inside `file` (an inline <style> in an HTML file).
    JASPERENGINE_API Stylesheet parseStylesheet(uint32_t file, std::string_view css, DiagnosticSink& sink,
                                                uint32_t baseOffset = 0);
    // A declaration list (style="", Lua style.cssText): nested rules are dropped with
    // HTMLUI-CSS-0008.
    JASPERENGINE_API std::vector<Declaration> parseDeclarationList(uint32_t file, std::string_view css, uint32_t baseOffset,
                                                                   DiagnosticSink& sink);

    // ---- the CSS Syntax entry points (§5.4), for tests and the rewriters. sink may be null;
    // errors, when given, receives every recovered error.

    // "parse a block's contents": declarations and rules of a block, in a Rule (kind Style,
    // no prelude, hasBlock false)
    JASPERENGINE_API Rule parseBlockContents(uint32_t file, std::string_view css, uint32_t baseOffset,
                                             DiagnosticSink* sink, std::vector<ParseError>* errors = nullptr);
    // "parse a stylesheet's contents" (topLevel: CDO/CDC skipped) or the 2021 draft's
    // "parse a list of rules" (CDO/CDC part of the rules)
    JASPERENGINE_API std::vector<Rule> parseRuleList(uint32_t file, std::string_view css, uint32_t baseOffset, bool topLevel,
                                                     DiagnosticSink* sink, std::vector<ParseError>* errors = nullptr);
    // "parse a rule": exactly one rule (Empty / ExtraInput otherwise)
    JASPERENGINE_API std::optional<Rule> parseRule(uint32_t file, std::string_view css, uint32_t baseOffset,
                                                   DiagnosticSink* sink, std::vector<ParseError>* errors = nullptr);
    // "parse a declaration"
    JASPERENGINE_API std::optional<Declaration> parseDeclaration(uint32_t file, std::string_view css, uint32_t baseOffset,
                                                                 DiagnosticSink* sink, std::vector<ParseError>* errors = nullptr);
    // "parse a list of component values" / "parse a component value"
    JASPERENGINE_API std::vector<ComponentValue> parseComponentValueList(std::string_view css, uint32_t baseOffset = 0,
                                                                         const TokenizeOptions& options = {},
                                                                         std::vector<ParseError>* errors = nullptr);
    JASPERENGINE_API std::optional<ComponentValue> parseComponentValue(std::string_view css, uint32_t baseOffset = 0,
                                                                       const TokenizeOptions& options = {},
                                                                       std::vector<ParseError>* errors = nullptr);

    // ---- helpers over component values

    // groups split at top-level commas (each group untrimmed)
    JASPERENGINE_API std::vector<std::vector<ComponentValue>> splitAtCommas(const std::vector<ComponentValue>& values);
    // without leading/trailing whitespace
    JASPERENGINE_API std::vector<ComponentValue> trimWhitespace(const std::vector<ComponentValue>& values);
    // the source extent of a sequence (invalid when empty)
    JASPERENGINE_API SourceRef extentOf(uint32_t file, const std::vector<ComponentValue>& values);

    JASPERENGINE_API const char* parseErrorName(ParseError::Kind kind);   // "invalid-rule", ...

}

#endif //COMPAT_CSS_CSSPARSER_H
