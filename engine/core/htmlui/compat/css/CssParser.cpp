// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/css/CssParser.h"
#include "htmlui/compat/LiteralCompare.h"

#include "htmlui/compat/Diagnostics.h"

#include <algorithm>

using namespace jasperengine::htmlui::compat;

namespace {

    std::string toLowerAscii(std::string text){
        for (char& c : text){
            if (c >= 'A' && c <= 'Z')
                c = (char)(c - 'A' + 'a');
        }
        return text;
    }

    uint32_t endOf(const ComponentValue& value){
        return value.offset + value.length;
    }

    TokenType mirrorOf(TokenType opening){
        switch (opening){
            case TokenType::LeftBrace: return TokenType::RightBrace;
            case TokenType::LeftBracket: return TokenType::RightBracket;
            default: return TokenType::RightParen;
        }
    }

    // CSS Syntax §5 over a token list (comments removed). Blocks of rules are kept raw (as
    // "consume a simple block" sees them) and parsed as block contents (2024 nesting).
    class Parser {
    public:
        Parser(uint32_t file, std::string_view css, uint32_t base, const TokenizeOptions& options,
               std::vector<ParseError>* errors):
            file(file), css(css), base(base), errorsOut(errors) {
            std::vector<TokenizeError> tokenizeErrors;
            tokens = tokenize(css, base, options, &tokenizeErrors);
            // comments removed in place
            size_t kept = 0;
            for (size_t i = 0; i < tokens.size(); i++){
                Token& token = tokens[i];
                if (token.type == TokenType::Comment)
                    continue;
                if (token.type == TokenType::BadString)
                    errors_.push_back(ParseError{ParseError::Kind::BadString, token.offset, token.length});
                else if (token.type == TokenType::BadUrl)
                    errors_.push_back(ParseError{ParseError::Kind::BadUrl, token.offset, token.length});
                if (kept != i)
                    tokens[kept] = std::move(token);
                kept++;
            }
            tokens.erase(tokens.begin() + (std::ptrdiff_t)kept, tokens.end());
            for (const TokenizeError& error : tokenizeErrors){
                ParseError::Kind kind = ParseError::Kind::InvalidEscape;
                switch (error.kind){
                    case TokenizeError::Kind::EofInString: kind = ParseError::Kind::EofInString; break;
                    case TokenizeError::Kind::EofInUrl: kind = ParseError::Kind::EofInUrl; break;
                    case TokenizeError::Kind::EofInComment: kind = ParseError::Kind::EofInComment; break;
                    case TokenizeError::Kind::EofInEscape:
                    case TokenizeError::Kind::InvalidEscape: kind = ParseError::Kind::InvalidEscape; break;
                }
                errors_.push_back(ParseError{kind, error.offset, 0});
            }
        }

        ~Parser(){
            if (errorsOut){
                std::stable_sort(errors_.begin(), errors_.end(), [](const ParseError& a, const ParseError& b){ return a.offset < b.offset; });
                errorsOut->insert(errorsOut->end(), errors_.begin(), errors_.end());
            }
        }

        // ---- token stream

        const Token& peek() const { return tokens[index]; }
        bool atEnd() const { return tokens[index].type == TokenType::EndOfFile; }
        const Token& next(){
            const Token& token = tokens[index];
            if (token.type != TokenType::EndOfFile)
                index++;
            return token;
        }
        void discardWhitespace(){
            while (peek().type == TokenType::Whitespace)
                index++;
        }
        uint32_t position() const { return peek().offset; }

        void error(ParseError::Kind kind, uint32_t offset, uint32_t length){
            errors_.push_back(ParseError{kind, offset, length});
        }

        // ---- component values (§5.5.8-5.5.10)

        ComponentValue tokenValue(const Token& token){
            ComponentValue value;
            value.kind = ComponentValue::Kind::Token;
            value.token = token;
            value.offset = token.offset;
            value.length = token.length;
            if (token.type == TokenType::RightParen || token.type == TokenType::RightBracket)
                error(ParseError::Kind::UnmatchedCloser, token.offset, token.length);
            return value;
        }

        ComponentValue consumeComponentValue(){
            const Token& token = peek();
            if (token.type == TokenType::LeftBrace || token.type == TokenType::LeftBracket || token.type == TokenType::LeftParen)
                return consumeSimpleBlock();
            if (token.type == TokenType::Function)
                return consumeFunction();
            return tokenValue(next());
        }

        ComponentValue consumeSimpleBlock(){
            ComponentValue block;
            block.kind = ComponentValue::Kind::Block;
            block.token = next();
            block.offset = block.token.offset;
            const TokenType ending = mirrorOf(block.token.type);
            while (true){
                const Token& token = peek();
                if (token.type == ending){
                    next();
                    block.length = token.offset + token.length - block.offset;
                    return block;
                }
                if (token.type == TokenType::EndOfFile){
                    block.closed = false;
                    error(ParseError::Kind::UnclosedBlock, block.token.offset, block.token.length);
                    block.length = (block.children.empty() ? block.token.offset + block.token.length : endOf(block.children.back())) - block.offset;
                    return block;
                }
                block.children.push_back(consumeComponentValue());
            }
        }

        ComponentValue consumeFunction(){
            ComponentValue function;
            function.kind = ComponentValue::Kind::Function;
            function.token = next();
            function.offset = function.token.offset;
            while (true){
                const Token& token = peek();
                if (token.type == TokenType::RightParen){
                    next();
                    function.length = token.offset + token.length - function.offset;
                    return function;
                }
                if (token.type == TokenType::EndOfFile){
                    function.closed = false;
                    error(ParseError::Kind::UnclosedFunction, function.token.offset, function.token.length);
                    function.length = (function.children.empty() ? function.token.offset + function.token.length : endOf(function.children.back())) - function.offset;
                    return function;
                }
                function.children.push_back(consumeComponentValue());
            }
        }

        // §5.5.7; stop not consumed
        std::vector<ComponentValue> consumeComponentValues(bool stopAtSemicolon, bool nested){
            std::vector<ComponentValue> values;
            while (true){
                const Token& token = peek();
                if (token.type == TokenType::EndOfFile)
                    return values;
                if (stopAtSemicolon && token.type == TokenType::Semicolon)
                    return values;
                if (token.type == TokenType::RightBrace){
                    if (nested)
                        return values;
                    error(ParseError::Kind::UnmatchedCloser, token.offset, token.length);
                    values.push_back(tokenValue(next()));
                    continue;
                }
                values.push_back(consumeComponentValue());
            }
        }

        // ---- rules (§5.5.1-5.5.6)

        SourceRef ref(uint32_t begin, uint32_t end) const {
            return SourceRef{file, begin, end > begin ? end - begin : 0};
        }

        SourceRef preludeExtent(const std::vector<ComponentValue>& prelude) const {
            return extentOf(file, trimWhitespace(prelude));
        }

        // at the '{': the raw block, then its contents parsed
        void consumeBlock(Rule& rule){
            const size_t open = index;
            rule.hasBlock = true;
            if (!keepRawBlocks){
                // the contents pass alone: it ends on the same closing brace
                const Token& opening = next();
                consumeBlockContents(rule.declarations, rule.rules);
                uint32_t end = opening.offset + opening.length;
                if (peek().type == TokenType::RightBrace){
                    const Token& closing = next();
                    end = closing.offset + closing.length;
                }else{
                    error(ParseError::Kind::UnclosedBlock, opening.offset, opening.length);
                    if (index > open + 1){
                        const Token& last = tokens[index - 1];
                        end = std::max(end, last.offset + last.length);
                    }
                }
                rule.blockRef = ref(opening.offset, end);
                return;
            }
            const size_t errorsBefore = errors_.size();
            ComponentValue raw = consumeSimpleBlock();
            const size_t afterRaw = index;
            // the contents pass reports its own errors for the same tokens
            errors_.resize(errorsBefore);

            index = open + 1;
            consumeBlockContents(rule.declarations, rule.rules);
            if (peek().type == TokenType::RightBrace)
                next();
            else
                error(ParseError::Kind::UnclosedBlock, raw.token.offset, raw.token.length);
            // both passes end on the same closing brace
            index = afterRaw;

            rule.block = std::move(raw.children);
            rule.blockRef = ref(raw.offset, raw.offset + raw.length);
        }

        std::optional<Rule> consumeAtRule(bool nested){
            Rule rule;
            rule.kind = Rule::Kind::At;
            const Token& keyword = next();
            rule.atName = toLowerAscii(keyword.value);
            const uint32_t begin = keyword.offset;
            uint32_t end = keyword.offset + keyword.length;

            while (true){
                const Token& token = peek();
                if (token.type == TokenType::Semicolon){
                    end = token.offset + token.length;
                    next();
                    break;
                }
                if (token.type == TokenType::EndOfFile)
                    break;
                if (token.type == TokenType::RightBrace){
                    if (nested)
                        break;
                    error(ParseError::Kind::UnmatchedCloser, token.offset, token.length);
                    rule.prelude.push_back(tokenValue(next()));
                    end = endOf(rule.prelude.back());
                    continue;
                }
                if (token.type == TokenType::LeftBrace){
                    consumeBlock(rule);
                    end = rule.blockRef.offset + rule.blockRef.length;
                    break;
                }
                rule.prelude.push_back(consumeComponentValue());
                end = endOf(rule.prelude.back());
            }

            rule.preludeRef = preludeExtent(rule.prelude);
            if (!rule.preludeRef.valid())
                rule.preludeRef = ref(keyword.offset + keyword.length, keyword.offset + keyword.length);
            rule.ref = ref(begin, end);
            return rule;
        }

        static bool looksLikeCustomProperty(const std::vector<ComponentValue>& prelude){
            size_t seen = 0;
            const ComponentValue* first = nullptr;
            for (const ComponentValue& value : prelude){
                if (value.isWhitespace())
                    continue;
                if (seen == 0){
                    first = &value;
                }else{
                    return first->isToken(TokenType::Ident) && first->token.value.size() >= 2 &&
                           first->token.value[0] == '-' && first->token.value[1] == '-' && value.isToken(TokenType::Colon);
                }
                seen++;
            }
            return false;
        }

        // §5.5.3; `invalid` set when a rule-shaped construct was dropped
        std::optional<Rule> consumeQualifiedRule(bool stopAtSemicolon, bool nested){
            Rule rule;
            rule.kind = Rule::Kind::Style;
            const uint32_t begin = position();

            while (true){
                const Token& token = peek();
                if (token.type == TokenType::EndOfFile || (stopAtSemicolon && token.type == TokenType::Semicolon)){
                    error(ParseError::Kind::InvalidRule, begin, (token.offset > begin ? token.offset - begin : 0));
                    return std::nullopt;
                }
                if (token.type == TokenType::RightBrace){
                    if (nested){
                        error(ParseError::Kind::InvalidRule, begin, (token.offset > begin ? token.offset - begin : 0));
                        return std::nullopt;
                    }
                    error(ParseError::Kind::UnmatchedCloser, token.offset, token.length);
                    rule.prelude.push_back(tokenValue(next()));
                    continue;
                }
                if (token.type == TokenType::LeftBrace){
                    if (looksLikeCustomProperty(rule.prelude)){
                        const uint32_t errorBegin = begin;
                        if (nested){
                            consumeBadDeclarationRemnants(true);
                        }else{
                            Rule dropped;
                            consumeBlock(dropped);
                        }
                        error(ParseError::Kind::InvalidRule, errorBegin, position() > errorBegin ? position() - errorBegin : 0);
                        return std::nullopt;
                    }
                    consumeBlock(rule);
                    rule.preludeRef = preludeExtent(rule.prelude);
                    if (!rule.preludeRef.valid())
                        rule.preludeRef = ref(rule.blockRef.offset, rule.blockRef.offset);
                    rule.ref = ref(begin, rule.blockRef.offset + rule.blockRef.length);
                    return rule;
                }
                rule.prelude.push_back(consumeComponentValue());
            }
        }

        void consumeBadDeclarationRemnants(bool nested){
            while (true){
                const Token& token = peek();
                if (token.type == TokenType::EndOfFile)
                    return;
                if (token.type == TokenType::Semicolon){
                    next();
                    return;
                }
                if (token.type == TokenType::RightBrace){
                    if (nested)
                        return;
                    next();
                    continue;
                }
                consumeComponentValue();
            }
        }

        // §5.5.6
        std::optional<Declaration> consumeDeclaration(bool nested){
            Declaration declaration;
            const Token& nameToken = peek();
            if (nameToken.type != TokenType::Ident){
                consumeBadDeclarationRemnants(nested);
                return std::nullopt;
            }
            next();
            const bool custom = nameToken.value.size() >= 2 && nameToken.value[0] == '-' && nameToken.value[1] == '-';
            declaration.name = custom ? nameToken.value : toLowerAscii(nameToken.value);
            declaration.nameRef = ref(nameToken.offset, nameToken.offset + nameToken.length);

            discardWhitespace();
            if (peek().type != TokenType::Colon){
                consumeBadDeclarationRemnants(nested);
                return std::nullopt;
            }
            const Token& colon = next();
            discardWhitespace();
            const uint32_t valueBegin = position() < colon.offset + colon.length ? colon.offset + colon.length : position();

            std::vector<ComponentValue> values = consumeComponentValues(true, nested);

            // trailing whitespace, then "! important"
            while (!values.empty() && values.back().isWhitespace())
                values.pop_back();
            uint32_t declarationEnd = values.empty() ? valueBegin : endOf(values.back());
            {
                size_t last = values.size();
                if (last >= 1 && values[last - 1].isToken(TokenType::Ident) && values[last - 1].token.isIdent("important")){
                    size_t bang = last - 1;
                    while (bang > 0 && values[bang - 1].isWhitespace())
                        bang--;
                    if (bang > 0 && values[bang - 1].isToken(TokenType::Delim) && values[bang - 1].token.isDelim('!')){
                        declaration.important = true;
                        values.erase(values.begin() + (std::ptrdiff_t)(bang - 1), values.end());
                    }
                }
            }
            while (!values.empty() && values.back().isWhitespace())
                values.pop_back();

            if (!custom){
                bool hasBraceBlock = false;
                bool hasOther = false;
                for (const ComponentValue& value : values){
                    if (value.isBlock(TokenType::LeftBrace))
                        hasBraceBlock = true;
                    else if (!value.isWhitespace())
                        hasOther = true;
                }
                if (hasBraceBlock && hasOther)
                    return std::nullopt;
            }

            if (!custom && declaration.name == "unicode-range" && !values.empty()){
                // the unicode-range descriptor: its source text again, with <unicode-range-token>s
                const uint32_t from = values.front().offset;
                const uint32_t to = endOf(values.back());
                TokenizeOptions options;
                options.unicodeRanges = true;
                std::vector<ComponentValue> ranges = parseComponentValueList(css.substr(from - base, to - from), from, options, nullptr);
                values = std::move(ranges);
            }

            declaration.value = std::move(values);
            declaration.valueRef = declaration.value.empty() ? ref(valueBegin, valueBegin)
                                                             : ref(declaration.value.front().offset, endOf(declaration.value.back()));
            declaration.ref = ref(nameToken.offset, declarationEnd);
            return declaration;
        }

        // §5.5.5, stops at '}' (not consumed) or EOF
        void consumeBlockContents(std::vector<Declaration>& declarations, std::vector<Rule>& rules){
            while (true){
                const Token& token = peek();
                if (token.type == TokenType::Whitespace || token.type == TokenType::Semicolon){
                    next();
                    continue;
                }
                if (token.type == TokenType::EndOfFile || token.type == TokenType::RightBrace)
                    return;
                if (token.type == TokenType::AtKeyword){
                    if (std::optional<Rule> rule = consumeAtRule(true))
                        rules.push_back(std::move(*rule));
                    continue;
                }

                const size_t mark = index;
                const size_t errorMark = errors_.size();
                const bool declarationShaped = looksLikeDeclarationStart();
                if (std::optional<Declaration> declaration = consumeDeclaration(true)){
                    declarations.push_back(std::move(*declaration));
                    continue;
                }
                index = mark;
                errors_.resize(errorMark);

                std::optional<Rule> rule = consumeQualifiedRule(true, true);
                if (rule){
                    rules.push_back(std::move(*rule));
                }else if (declarationShaped && !errors_.empty() && errors_.back().kind == ParseError::Kind::InvalidRule){
                    errors_.back().kind = ParseError::Kind::InvalidDeclaration;
                }
            }
        }

        // an ident followed by a colon: reported as a bad declaration rather than a bad rule
        bool looksLikeDeclarationStart() const {
            if (tokens[index].type != TokenType::Ident)
                return false;
            size_t i = index + 1;
            while (tokens[i].type == TokenType::Whitespace)
                i++;
            return tokens[i].type == TokenType::Colon;
        }

        // §5.5.1 (topLevel) or the 2021 draft's "consume a list of rules"
        std::vector<Rule> consumeRuleList(bool topLevel){
            std::vector<Rule> rules;
            while (true){
                const Token& token = peek();
                if (token.type == TokenType::Whitespace){
                    next();
                    continue;
                }
                if (token.type == TokenType::EndOfFile)
                    return rules;
                if (topLevel && (token.type == TokenType::CDO || token.type == TokenType::CDC)){
                    next();
                    continue;
                }
                if (token.type == TokenType::AtKeyword){
                    if (std::optional<Rule> rule = consumeAtRule(false))
                        rules.push_back(std::move(*rule));
                    continue;
                }
                if (std::optional<Rule> rule = consumeQualifiedRule(false, false))
                    rules.push_back(std::move(*rule));
            }
        }

        void report(DiagnosticSink* sink) const {
            if (!sink)
                return;
            // a bad string or url already explains the declaration or rule it breaks
            std::vector<uint32_t> badTokens;
            for (const ParseError& error : errors_){
                if (error.kind == ParseError::Kind::BadString || error.kind == ParseError::Kind::BadUrl)
                    badTokens.push_back(error.offset);
            }
            for (const ParseError& error : errors_){
                if ((error.kind == ParseError::Kind::InvalidRule || error.kind == ParseError::Kind::InvalidDeclaration) &&
                    std::find(badTokens.begin(), badTokens.end(), error.offset) != badTokens.end())
                    continue;
                const SourceRef at{file, error.offset, error.length};
                const std::string_view text = error.length > 0 && error.offset >= base && error.offset - base < css.size()
                    ? css.substr(error.offset - base, std::min<size_t>(error.length, css.size() - (error.offset - base)))
                    : std::string_view();
                std::string quoted(text.substr(0, std::min<size_t>(text.size(), 60)));
                for (char& c : quoted){
                    if (c == '\n' || c == '\r' || c == '\t')
                        c = ' ';
                }
                if (text.size() > 60)
                    quoted += "...";
                std::string message;
                std::string suggestion;
                switch (error.kind){
                    case ParseError::Kind::InvalidRule:
                        message = quoted.empty() ? "invalid rule dropped" : "invalid rule '" + quoted + "' dropped";
                        suggestion = "give the selector a { ... } block";
                        break;
                    case ParseError::Kind::InvalidDeclaration:
                        message = quoted.empty() ? "invalid declaration dropped" : "invalid declaration '" + quoted + "' dropped";
                        suggestion = "write declarations as 'property: value;'";
                        break;
                    case ParseError::Kind::UnclosedBlock:
                    case ParseError::Kind::UnclosedFunction:
                        message = "'" + quoted + "' is not closed before the end of the stylesheet";
                        suggestion = "add the missing closing bracket";
                        break;
                    case ParseError::Kind::UnmatchedCloser:
                        message = "unmatched '" + quoted + "'";
                        suggestion = "remove it or add the opening bracket";
                        break;
                    case ParseError::Kind::BadString:
                        message = "a string is not closed before the end of the line";
                        suggestion = "close the quote, or escape the line break with '\\'";
                        break;
                    case ParseError::Kind::BadUrl:
                        message = "invalid url(" + std::string(quoted.size() > 4 ? quoted.substr(4) : std::string()) + "; quotes, '(' and spaces need a quoted URL";
                        suggestion = "quote the URL: url(\"...\")";
                        break;
                    case ParseError::Kind::EofInString:
                        message = "a string is not closed before the end of the stylesheet";
                        suggestion = "close the quote";
                        break;
                    case ParseError::Kind::EofInUrl:
                        message = "url( is not closed before the end of the stylesheet";
                        suggestion = "add the closing ')'";
                        break;
                    case ParseError::Kind::EofInComment:
                        message = "a comment is not closed before the end of the stylesheet";
                        suggestion = "close it with '*/'";
                        break;
                    case ParseError::Kind::InvalidEscape:
                        message = "invalid escape '\\' (a backslash before a line break or at the end)";
                        suggestion = "remove the backslash";
                        break;
                    case ParseError::Kind::Empty:
                        message = "nothing to parse";
                        break;
                    case ParseError::Kind::ExtraInput:
                        message = "unexpected input after the end";
                        break;
                }
                sink->report(codes::CssSyntaxError, at, message, suggestion);
            }
        }

        uint32_t file;
        std::string_view css;
        uint32_t base;
        std::vector<Token> tokens;
        size_t index = 0;
        // Rule::block (the raw block) is kept for the CSS Syntax entry points; the
        // compatibility layer's parseStylesheet/parseDeclarationList only use the contents
        bool keepRawBlocks = true;
        std::vector<ParseError> errors_;
        std::vector<ParseError>* errorsOut;
    };

}

SourceRef jasperengine::htmlui::compat::extentOf(uint32_t file, const std::vector<ComponentValue>& values){
    if (values.empty())
        return SourceRef{};
    const uint32_t begin = values.front().offset;
    const uint32_t end = endOf(values.back());
    return SourceRef{file, begin, end > begin ? end - begin : 0};
}

std::vector<ComponentValue> jasperengine::htmlui::compat::trimWhitespace(const std::vector<ComponentValue>& values){
    size_t begin = 0;
    size_t end = values.size();
    while (begin < end && values[begin].isWhitespace())
        begin++;
    while (end > begin && values[end - 1].isWhitespace())
        end--;
    return std::vector<ComponentValue>(values.begin() + (std::ptrdiff_t)begin, values.begin() + (std::ptrdiff_t)end);
}

std::vector<std::vector<ComponentValue>> jasperengine::htmlui::compat::splitAtCommas(const std::vector<ComponentValue>& values){
    std::vector<std::vector<ComponentValue>> groups(1);
    for (const ComponentValue& value : values){
        if (value.isToken(TokenType::Comma)){
            groups.emplace_back();
            continue;
        }
        groups.back().push_back(value);
    }
    return groups;
}

Stylesheet jasperengine::htmlui::compat::parseStylesheet(uint32_t file, std::string_view css, DiagnosticSink& sink, uint32_t baseOffset){
    Stylesheet sheet;
    sheet.file = file;
    Parser parser(file, css, baseOffset, TokenizeOptions(), nullptr);
    parser.keepRawBlocks = false;
    sheet.rules = parser.consumeRuleList(true);
    parser.report(&sink);
    return sheet;
}

std::vector<Declaration> jasperengine::htmlui::compat::parseDeclarationList(uint32_t file, std::string_view css, uint32_t baseOffset,
                                                                            DiagnosticSink& sink){
    Parser parser(file, css, baseOffset, TokenizeOptions(), nullptr);
    parser.keepRawBlocks = false;
    std::vector<Declaration> declarations;
    std::vector<Rule> rules;
    while (true){
        parser.consumeBlockContents(declarations, rules);
        if (parser.atEnd())
            break;
        // a stray '}' ends nothing in a declaration list
        const Token& closer = parser.next();
        parser.error(ParseError::Kind::UnmatchedCloser, closer.offset, closer.length);
    }
    for (const Rule& rule : rules)
        parser.error(ParseError::Kind::InvalidDeclaration, rule.ref.offset, rule.ref.length);
    parser.report(&sink);
    return declarations;
}

Rule jasperengine::htmlui::compat::parseBlockContents(uint32_t file, std::string_view css, uint32_t baseOffset,
                                                      DiagnosticSink* sink, std::vector<ParseError>* errors){
    Rule result;
    result.kind = Rule::Kind::Style;
    Parser parser(file, css, baseOffset, TokenizeOptions(), errors);
    while (true){
        parser.consumeBlockContents(result.declarations, result.rules);
        if (parser.atEnd())
            break;
        const Token& closer = parser.next();
        parser.error(ParseError::Kind::UnmatchedCloser, closer.offset, closer.length);
    }
    result.ref = SourceRef{file, baseOffset, (uint32_t)css.size()};
    parser.report(sink);
    return result;
}

std::vector<Rule> jasperengine::htmlui::compat::parseRuleList(uint32_t file, std::string_view css, uint32_t baseOffset, bool topLevel,
                                                              DiagnosticSink* sink, std::vector<ParseError>* errors){
    Parser parser(file, css, baseOffset, TokenizeOptions(), errors);
    std::vector<Rule> rules = parser.consumeRuleList(topLevel);
    parser.report(sink);
    return rules;
}

std::optional<Rule> jasperengine::htmlui::compat::parseRule(uint32_t file, std::string_view css, uint32_t baseOffset,
                                                            DiagnosticSink* sink, std::vector<ParseError>* errors){
    Parser parser(file, css, baseOffset, TokenizeOptions(), errors);
    parser.discardWhitespace();
    std::optional<Rule> rule;
    if (parser.atEnd()){
        parser.error(ParseError::Kind::Empty, baseOffset, 0);
    }else{
        if (parser.peek().type == TokenType::AtKeyword)
            rule = parser.consumeAtRule(false);
        else
            rule = parser.consumeQualifiedRule(false, false);
        if (rule){
            parser.discardWhitespace();
            if (!parser.atEnd()){
                parser.error(ParseError::Kind::ExtraInput, parser.position(), 0);
                rule.reset();
            }
        }
    }
    parser.report(sink);
    return rule;
}

std::optional<Declaration> jasperengine::htmlui::compat::parseDeclaration(uint32_t file, std::string_view css, uint32_t baseOffset,
                                                                          DiagnosticSink* sink, std::vector<ParseError>* errors){
    Parser parser(file, css, baseOffset, TokenizeOptions(), errors);
    parser.discardWhitespace();
    std::optional<Declaration> declaration;
    if (parser.atEnd()){
        parser.error(ParseError::Kind::Empty, baseOffset, 0);
    }else{
        const uint32_t begin = parser.position();
        declaration = parser.consumeDeclaration(false);
        if (!declaration)
            parser.error(ParseError::Kind::InvalidDeclaration, begin, parser.position() > begin ? parser.position() - begin : 0);
    }
    parser.report(sink);
    return declaration;
}

std::vector<ComponentValue> jasperengine::htmlui::compat::parseComponentValueList(std::string_view css, uint32_t baseOffset,
                                                                                  const TokenizeOptions& options,
                                                                                  std::vector<ParseError>* errors){
    Parser parser(0, css, baseOffset, options, errors);
    return parser.consumeComponentValues(false, false);
}

std::optional<ComponentValue> jasperengine::htmlui::compat::parseComponentValue(std::string_view css, uint32_t baseOffset,
                                                                                const TokenizeOptions& options,
                                                                                std::vector<ParseError>* errors){
    Parser parser(0, css, baseOffset, options, errors);
    parser.discardWhitespace();
    if (parser.atEnd()){
        parser.error(ParseError::Kind::Empty, baseOffset, 0);
        return std::nullopt;
    }
    ComponentValue value = parser.consumeComponentValue();
    parser.discardWhitespace();
    if (!parser.atEnd()){
        parser.error(ParseError::Kind::ExtraInput, parser.position(), 0);
        return std::nullopt;
    }
    return value;
}

const char* jasperengine::htmlui::compat::parseErrorName(ParseError::Kind kind){
    switch (kind){
        case ParseError::Kind::InvalidRule: return "invalid-rule";
        case ParseError::Kind::InvalidDeclaration: return "invalid-declaration";
        case ParseError::Kind::UnclosedBlock: return "unclosed-block";
        case ParseError::Kind::UnclosedFunction: return "unclosed-function";
        case ParseError::Kind::UnmatchedCloser: return "unmatched-closer";
        case ParseError::Kind::BadString: return "bad-string";
        case ParseError::Kind::BadUrl: return "bad-url";
        case ParseError::Kind::EofInString: return "eof-in-string";
        case ParseError::Kind::EofInUrl: return "eof-in-url";
        case ParseError::Kind::EofInComment: return "eof-in-comment";
        case ParseError::Kind::InvalidEscape: return "invalid-escape";
        case ParseError::Kind::Empty: return "empty";
        case ParseError::Kind::ExtraInput: return "extra-input";
    }
    return "?";
}
