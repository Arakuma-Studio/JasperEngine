// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HtmlCssLexer.h"

#include <algorithm>
#include <cstring>
#include <initializer_list>

namespace jasperengine::editor {

namespace {

using Kind = HtmlCssLexer::Kind;
using Token = HtmlCssLexer::Token;
using State = HtmlCssLexer::State;

bool isAlpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isHexDigit(char c) { return isDigit(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'); }
bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }
bool isNonAscii(char c) { return static_cast<unsigned char>(c) >= 0x80; }
char toLowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// line[start, end) is the lowercase word, ignoring ASCII case
bool equalsIgnoreCase(const std::string& line, int start, int end, const char* word) {
    int i = start;
    for (; *word; ++word, ++i) {
        if (i >= end || toLowerAscii(line[i]) != *word) return false;
    }
    return i == end;
}

// Where the lowercase needle first starts at or after from, ignoring ASCII case; -1 if nowhere
int findIgnoreCase(const std::string& line, int from, const char* needle) {
    const int len = static_cast<int>(line.size());
    const int needleLen = static_cast<int>(std::strlen(needle));
    for (int i = from; i + needleLen <= len; ++i) {
        if (equalsIgnoreCase(line, i, i + needleLen, needle)) return i;
    }
    return -1;
}

void push(std::vector<Token>& tokens, int start, int end, Kind kind) {
    if (end > start) tokens.push_back({start, end - start, kind});
}

// -- Css --

// Names without escapes; non-ASCII bytes are name characters, so a token never splits UTF-8
bool isCssNameStart(char c) { return isAlpha(c) || c == '_' || isNonAscii(c); }
bool isCssNameChar(char c) { return isCssNameStart(c) || isDigit(c) || c == '-'; }

bool startsCssIdent(const std::string& line, int i, int end) {
    if (i >= end) return false;
    if (isCssNameStart(line[i])) return true;
    return line[i] == '-' && i + 1 < end && (isCssNameStart(line[i + 1]) || line[i + 1] == '-');
}

int scanCssName(const std::string& line, int i, int end) {
    while (i < end && isCssNameChar(line[i])) ++i;
    return i;
}

bool startsCssNumber(const std::string& line, int i, int end) {
    if (i < end && (line[i] == '+' || line[i] == '-')) ++i;
    if (i < end && isDigit(line[i])) return true;
    return i + 1 < end && line[i] == '.' && isDigit(line[i + 1]);
}

// A number with its unit: 12px, -.5em, 50%, 1e3
int scanCssNumber(const std::string& line, int i, int end) {
    if (line[i] == '+' || line[i] == '-') ++i;
    while (i < end && isDigit(line[i])) ++i;
    if (i + 1 < end && line[i] == '.' && isDigit(line[i + 1])) {
        ++i;
        while (i < end && isDigit(line[i])) ++i;
    }
    if (i < end && (line[i] == 'e' || line[i] == 'E')) {
        int j = i + 1;
        if (j < end && (line[j] == '+' || line[j] == '-')) ++j;
        if (j < end && isDigit(line[j])) {
            i = j;
            while (i < end && isDigit(line[i])) ++i;
        }
    }
    if (i < end && line[i] == '%') return i + 1;
    if (startsCssIdent(line, i, end)) return scanCssName(line, i, end);
    return i;
}

// From the opening quote; an unterminated string ends with the range
int scanCssString(const std::string& line, int i, int end) {
    const char quote = line[i++];
    while (i < end && line[i] != quote) {
        i += (line[i] == '\\' && i + 1 < end) ? 2 : 1;
    }
    return i < end ? i + 1 : end;
}

// From inside a comment to just past its "*/", or to the range end if it goes on
int scanCssCommentBody(const std::string& line, int i, int end, bool& inComment) {
    for (; i + 1 < end; ++i) {
        if (line[i] == '*' && line[i + 1] == '/') {
            inComment = false;
            return i + 2;
        }
    }
    return end;
}

// At-rules whose block holds rules (@media screen { .a { ... } }) rather than declarations
bool atRuleHoldsRules(const std::string& line, int start, int end) {
    // Vendor prefix: @-webkit-keyframes
    if (start < end && line[start] == '-') {
        int dash = start + 1;
        while (dash < end && line[dash] != '-') ++dash;
        if (dash < end) start = dash + 1;
    }
    for (const char* name : {"media", "supports", "document", "layer", "container", "scope", "keyframes"}) {
        if (equalsIgnoreCase(line, start, end, name)) return true;
    }
    return false;
}

bool isDeclarationBlock(int depth, uint32_t declarationBlocks) {
    if (depth <= 0) return false;
    // Deeper blocks are not recorded, a rule's block is the likely one
    if (depth > 32) return true;
    return ((declarationBlocks >> (depth - 1)) & 1u) != 0;
}

// The CSS state one lexCss call works on: a stylesheet's, or a style="" value's
struct CssContext {
    bool& inComment;
    bool& inValue;
    bool& inAtRulePrelude;
    bool& atRuleHoldsRules;
    int& depth;
    uint32_t& declarationBlocks;
    bool declarationsOnly; // a style="" value: declarations and no blocks
};

CssContext stylesheetContext(State& state) {
    return {state.inCssComment, state.cssInValue, state.cssInAtRulePrelude, state.cssAtRuleHoldsRules,
            state.cssDepth, state.cssDeclarationBlocks, false};
}

void lexCss(const std::string& line, int i, int end, CssContext css, std::vector<Token>& tokens) {
    while (i < end) {
        if (css.inComment) {
            const int stop = scanCssCommentBody(line, i, end, css.inComment);
            push(tokens, i, stop, Kind::Comment);
            i = stop;
            continue;
        }

        const char c = line[i];
        if (isSpace(c)) {
            ++i;
            continue;
        }

        if (c == '/' && i + 1 < end && line[i + 1] == '*') {
            css.inComment = true;
            const int stop = scanCssCommentBody(line, i + 2, end, css.inComment);
            push(tokens, i, stop, Kind::Comment);
            i = stop;
            continue;
        }

        if (c == '"' || c == '\'') {
            const int stop = scanCssString(line, i, end);
            push(tokens, i, stop, Kind::String);
            i = stop;
            continue;
        }

        if (c == '@' && startsCssIdent(line, i + 1, end)) {
            const int stop = scanCssName(line, i + 1, end);
            push(tokens, i, stop, Kind::Preprocessor);
            css.inAtRulePrelude = true;
            css.atRuleHoldsRules = atRuleHoldsRules(line, i + 1, stop);
            css.inValue = false;
            i = stop;
            continue;
        }

        if (c == '!') {
            int j = i + 1;
            while (j < end && isSpace(line[j])) ++j;
            const int stop = j + 9;
            if (stop <= end && equalsIgnoreCase(line, j, stop, "important") &&
                !(stop < end && isCssNameChar(line[stop]))) {
                push(tokens, i, stop, Kind::Keyword);
                i = stop;
            } else {
                ++i;
            }
            continue;
        }

        if (c == '{' || c == '}' || c == ';') {
            if (!css.declarationsOnly) {
                if (c == '{') {
                    const bool declarations = !(css.inAtRulePrelude && css.atRuleHoldsRules);
                    ++css.depth;
                    if (css.depth <= 32) {
                        const uint32_t bit = 1u << (css.depth - 1);
                        css.declarationBlocks = declarations ? (css.declarationBlocks | bit) : (css.declarationBlocks & ~bit);
                    }
                } else if (c == '}' && css.depth > 0) {
                    --css.depth;
                }
            }
            css.inValue = false;
            css.inAtRulePrelude = false;
            ++i;
            continue;
        }

        const bool declarations = !css.inAtRulePrelude &&
                                  (css.declarationsOnly || isDeclarationBlock(css.depth, css.declarationBlocks));

        // Before the value only "name:" matters, the name is a property
        if (declarations && !css.inValue) {
            if (startsCssIdent(line, i, end)) {
                const int stop = scanCssName(line, i, end);
                int j = stop;
                while (j < end && isSpace(line[j])) ++j;
                if (j < end && line[j] == ':') push(tokens, i, stop, Kind::Type);
                i = stop;
            } else {
                if (c == ':') css.inValue = true;
                ++i;
            }
            continue;
        }

        // Selectors, at-rule preludes and declaration values from here on
        const bool selectors = !declarations && !css.inAtRulePrelude;

        if (startsCssIdent(line, i, end)) {
            const int stop = scanCssName(line, i, end);
            if (stop < end && line[stop] == '(' && equalsIgnoreCase(line, i, stop, "url")) {
                // An unquoted url(path) is a string, a quoted one is lexed as one next
                int j = stop + 1;
                while (j < end && isSpace(line[j])) ++j;
                if (j < end && line[j] != '"' && line[j] != '\'' && line[j] != ')') {
                    int close = j;
                    while (close < end && line[close] != ')') ++close;
                    push(tokens, j, close, Kind::String);
                    i = close;
                } else {
                    i = j;
                }
                continue;
            }
            // Type selectors take the colour of Html tag names
            if (selectors) push(tokens, i, stop, Kind::Keyword);
            i = stop;
            continue;
        }

        if (startsCssNumber(line, i, end)) {
            const int stop = scanCssNumber(line, i, end);
            push(tokens, i, stop, Kind::Number);
            i = stop;
            continue;
        }

        if (c == '#') {
            const int stop = scanCssName(line, i + 1, end);
            if (selectors && stop > i + 1) {
                push(tokens, i, stop, Kind::Builtin);
            } else if (declarations) {
                // #rgb, #rgba, #rrggbb, #rrggbbaa
                const int digits = stop - (i + 1);
                bool hex = digits == 3 || digits == 4 || digits == 6 || digits == 8;
                for (int j = i + 1; hex && j < stop; ++j) hex = isHexDigit(line[j]);
                if (hex) push(tokens, i, stop, Kind::Number);
            }
            i = stop;
            continue;
        }

        // .class, :pseudo, ::pseudo
        if (selectors && (c == '.' || c == ':')) {
            int j = i + 1;
            if (c == ':' && j < end && line[j] == ':') ++j;
            if (startsCssIdent(line, j, end)) {
                const int stop = scanCssName(line, j, end);
                push(tokens, i, stop, Kind::Builtin);
                i = stop;
                continue;
            }
        }

        ++i;
    }
}

// -- Html --

bool isHtmlNameStart(char c) { return isAlpha(c) || c == '_' || c == ':'; }
bool isHtmlNameChar(char c) { return isHtmlNameStart(c) || isDigit(c) || c == '-' || c == '.'; }

int scanHtmlName(const std::string& line, int i, int len) {
    while (i < len && isHtmlNameChar(line[i])) ++i;
    return i;
}

// &name; &#123; &#x1F;: just past the ';', or i if no entity starts there
int scanEntity(const std::string& line, int i, int len) {
    int j = i + 1;
    if (j < len && line[j] == '#') {
        ++j;
        const bool hex = j < len && (line[j] == 'x' || line[j] == 'X');
        if (hex) ++j;
        const int digitsStart = j;
        while (j < len && (hex ? isHexDigit(line[j]) : isDigit(line[j]))) ++j;
        if (j == digitsStart) return i;
    } else {
        if (j >= len || !isAlpha(line[j])) return i;
        while (j < len && (isAlpha(line[j]) || isDigit(line[j]))) ++j;
    }
    return (j < len && line[j] == ';') ? j + 1 : i;
}

// A quoted attribute value from `from` through its closing quote, or to the end of the line if
// it goes on. tokenStart is the opening quote, or `from` on a continuation line. Returns where it stops
int lexAttributeValue(const std::string& line, int tokenStart, int from, int len, State& state,
                      std::vector<Token>& tokens) {
    const size_t closePos = line.find(state.attributeQuote, static_cast<size_t>(from));
    const bool closed = closePos != std::string::npos;
    const int close = closed ? static_cast<int>(closePos) : len;
    const int stop = closed ? close + 1 : len;

    if (state.attributeIsStyle) {
        // Declarations only, so the block state is a throwaway
        bool inAtRulePrelude = false;
        bool atRuleHoldsRules = false;
        int depth = 0;
        uint32_t declarationBlocks = 0;
        push(tokens, tokenStart, from, Kind::String);
        lexCss(line, from, close,
               {state.styleAttributeInComment, state.styleAttributeInValue, inAtRulePrelude, atRuleHoldsRules,
                depth, declarationBlocks, true},
               tokens);
        push(tokens, close, stop, Kind::String);
    } else {
        push(tokens, tokenStart, stop, Kind::String);
    }

    if (closed) {
        state.attributeQuote = 0;
        state.attributeIsStyle = false;
    }
    return stop;
}

// Inside a tag, from i; returns where the next piece starts
int lexTag(const std::string& line, int i, int len, State& state, std::vector<Token>& tokens) {
    const char c = line[i];

    if (isSpace(c)) return i + 1;

    if (c == '>' || (c == '/' && i + 1 < len && line[i + 1] == '>')) {
        const bool selfClosing = c == '/';
        const int stop = selfClosing ? i + 2 : i + 1;
        push(tokens, i, stop, Kind::Keyword);
        if (!selfClosing && state.tagOpensStyle) {
            // Each style block is a stylesheet of its own
            state.inStyleBlock = true;
            state.inCssComment = false;
            state.cssInValue = false;
            state.cssInAtRulePrelude = false;
            state.cssAtRuleHoldsRules = false;
            state.cssDepth = 0;
            state.cssDeclarationBlocks = 0;
        }
        if (!selfClosing && state.tagOpensScript) state.inScriptBlock = true;
        state.inHtmlTag = false;
        state.tagOpensStyle = false;
        state.tagOpensScript = false;
        state.expectingAttributeValue = false;
        state.attributeIsStyle = false;
        return stop;
    }

    if (c == '"' || c == '\'') {
        state.attributeQuote = c;
        state.expectingAttributeValue = false;
        state.styleAttributeInValue = false;
        state.styleAttributeInComment = false;
        return lexAttributeValue(line, i, i + 1, len, state, tokens);
    }

    if (state.expectingAttributeValue) {
        // Unquoted value
        int stop = i;
        while (stop < len && !isSpace(line[stop]) && line[stop] != '"' && line[stop] != '\'' && line[stop] != '=' &&
               line[stop] != '<' && line[stop] != '>' && line[stop] != '`') {
            ++stop;
        }
        state.expectingAttributeValue = false;
        state.attributeIsStyle = false;
        if (stop == i) return i + 1;
        push(tokens, i, stop, Kind::String);
        return stop;
    }

    if (c == '=') {
        state.expectingAttributeValue = true;
        return i + 1;
    }

    if (isHtmlNameStart(c)) {
        const int stop = scanHtmlName(line, i, len);
        push(tokens, i, stop, Kind::Type);
        state.attributeIsStyle = equalsIgnoreCase(line, i, stop, "style");
        return stop;
    }

    return i + 1;
}

void lexHtml(const std::string& line, State& state, std::vector<Token>& tokens) {
    const int len = static_cast<int>(line.size());
    int i = 0;

    while (i < len) {
        if (state.inHtmlComment) {
            const size_t close = line.find("-->", static_cast<size_t>(i));
            const int stop = close == std::string::npos ? len : static_cast<int>(close) + 3;
            if (close != std::string::npos) state.inHtmlComment = false;
            push(tokens, i, stop, Kind::Comment);
            i = stop;
            continue;
        }

        if (state.attributeQuote != 0) {
            i = lexAttributeValue(line, i, i, len, state, tokens);
            continue;
        }

        if (state.inHtmlTag) {
            i = lexTag(line, i, len, state, tokens);
            continue;
        }

        if (state.inStyleBlock) {
            const int close = findIgnoreCase(line, i, "</style");
            const int stop = close < 0 ? len : close;
            lexCss(line, i, stop, stylesheetContext(state), tokens);
            if (close >= 0) state.inStyleBlock = false;
            i = stop;
            continue;
        }

        if (state.inScriptBlock) {
            const int close = findIgnoreCase(line, i, "</script");
            if (close < 0) break;
            state.inScriptBlock = false;
            i = close;
            continue;
        }

        const char c = line[i];

        if (c == '<') {
            if (line.compare(i, 4, "<!--") == 0) {
                state.inHtmlComment = true;
                const size_t close = line.find("-->", static_cast<size_t>(i) + 4);
                const int stop = close == std::string::npos ? len : static_cast<int>(close) + 3;
                if (close != std::string::npos) state.inHtmlComment = false;
                push(tokens, i, stop, Kind::Comment);
                i = stop;
                continue;
            }

            // <!DOCTYPE html>, <?xml ... ?>
            if (i + 1 < len && (line[i + 1] == '!' || line[i + 1] == '?')) {
                const size_t close = line.find('>', static_cast<size_t>(i));
                const int stop = close == std::string::npos ? len : static_cast<int>(close) + 1;
                push(tokens, i, stop, Kind::Preprocessor);
                i = stop;
                continue;
            }

            const bool closing = i + 1 < len && line[i + 1] == '/';
            const int nameStart = i + (closing ? 2 : 1);
            if (nameStart < len && isAlpha(line[nameStart])) {
                const int stop = scanHtmlName(line, nameStart, len);
                push(tokens, i, stop, Kind::Keyword);
                state.inHtmlTag = true;
                state.tagOpensStyle = !closing && equalsIgnoreCase(line, nameStart, stop, "style");
                state.tagOpensScript = !closing && equalsIgnoreCase(line, nameStart, stop, "script");
                state.expectingAttributeValue = false;
                state.attributeIsStyle = false;
                i = stop;
                continue;
            }
        }

        if (c == '&') {
            const int stop = scanEntity(line, i, len);
            if (stop > i) {
                push(tokens, i, stop, Kind::Builtin);
                i = stop;
                continue;
            }
        }

        ++i;
    }
}

} // namespace

std::vector<HtmlCssLexer::Token> HtmlCssLexer::lexLine(Language language, const std::string& line, State& state) {
    std::vector<Token> tokens;
    if (language == Language::Css) {
        lexCss(line, 0, static_cast<int>(line.size()), stylesheetContext(state), tokens);
    } else {
        lexHtml(line, state, tokens);
    }
    return tokens;
}

HtmlCssLexer::NamePosition HtmlCssLexer::htmlNamePosition(const std::string& line, int column, State lineState) {
    column = std::max(0, std::min<int>(column, static_cast<int>(line.size())));
    if (column >= 1 && line[column - 1] == '<') return NamePosition::Tag;
    if (column >= 2 && line[column - 1] == '/' && line[column - 2] == '<') return NamePosition::Tag;
    lexLine(Language::Html, line.substr(0, column), lineState);
    const bool afterSpace = column == 0 || line[column - 1] == ' ' || line[column - 1] == '\t';
    if (lineState.inHtmlTag && !lineState.expectingAttributeValue && lineState.attributeQuote == 0 && afterSpace)
        return NamePosition::Attribute;
    return NamePosition::None;
}

bool HtmlCssLexer::expectsCssProperty(Language language, const State& state) {
    if (language == Language::Html) {
        if (state.attributeQuote != 0 && state.attributeIsStyle) {
            return !state.styleAttributeInValue && !state.styleAttributeInComment;
        }
        if (!state.inStyleBlock) return false;
    }
    return !state.inCssComment && !state.cssInValue && !state.cssInAtRulePrelude &&
           isDeclarationBlock(state.cssDepth, state.cssDeclarationBlocks);
}

} // namespace jasperengine::editor
