// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/css/CssParser.h"
#include "htmlui/compat/LiteralCompare.h"

#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/css/CssLexer.h"
#include "htmlui/compat/css/Units.h"

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

    // how Parser::deferPrelude reads a byte of a prelude
    enum class PreludeByte : uint8_t { Other, Plain, Space, Open };
    struct PreludeBytes {
        PreludeByte kind[256];
        constexpr PreludeBytes(): kind() {
            for (int c = 0; c < 256; c++){
                PreludeByte k = PreludeByte::Other;
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '#' ||
                    c == ':' || c == '>' || c == '+' || c == '~' || c == '*' || c == ',')
                    k = PreludeByte::Plain;
                else if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f')
                    k = PreludeByte::Space;
                else if (c == '{')
                    k = PreludeByte::Open;
                kind[c] = k;
            }
        }
    };
    inline constexpr PreludeBytes kPreludeBytes{};

    // What the parsers of a thread are building: the items of the lists still open (a block's
    // children, a value, a prelude, a block's declarations and rules), above the mark each list
    // took when it opened; a list closes into the arena at its exact size (ArenaList::movedFrom)
    // and leaves the stacks as they were. A parser started while another one is building (the
    // unicode-range descriptor parsed again) works above it.
    struct Scratch {
        std::vector<ComponentValue> values;
        std::vector<Declaration> declarations;
        std::vector<Rule> rules;
    };
    thread_local Scratch tScratch;

    template<typename T>
    ArenaList<T> takeFrom(std::vector<T>& stack, size_t mark){
        ArenaList<T> list = ArenaList<T>::movedFrom(stack.data() + mark, stack.size() - mark);
        // (nothing to destroy: the items live on in the list)
        stack.resize(mark);
        return list;
    }

    // CSS Syntax §5 over the token stream of a CssLexer (comments dropped): every token is read
    // once, into the parser's lookahead and from there into the component value it is. Blocks
    // of rules are kept raw (as "consume a simple block" sees them) only for the CSS Syntax
    // entry points, and parsed as block contents (2024 nesting).
    class Parser {
    public:
        Parser(uint32_t file, std::string_view css, uint32_t base, const TokenizeOptions& options,
               std::vector<ParseError>* errors):
            file(file), css(css), base(base),
            // the tokens' texts are views into a copy of the source that lives with the parsed
            // values (the caller's text may not)
            sourceCopy(Arena::current().copy(css.data(), css.size())),
            lexer(std::string_view(sourceCopy, css.size()), base, options, false), errorsOut(errors),
            scratch(tScratch), valuesMark(tScratch.values.size()), declarationsMark(tScratch.declarations.size()),
            rulesMark(tScratch.rules.size()) {
            lexer.next(look);
        }

        ~Parser(){
            if (errorsOut){
                std::vector<ParseError> all = allErrors();
                std::stable_sort(all.begin(), all.end(), [](const ParseError& a, const ParseError& b){ return a.offset < b.offset; });
                errorsOut->insert(errorsOut->end(), all.begin(), all.end());
            }
            // (every path closes what it opened; this only guards the thread's stacks)
            scratch.values.resize(valuesMark);
            scratch.declarations.resize(declarationsMark);
            scratch.rules.resize(rulesMark);
        }

        // ---- token stream

        const Token& peek() const { return look; }
        bool atEnd() const { return look.type == TokenType::EndOfFile; }
        void skip(){
            if (look.type != TokenType::EndOfFile){
                lastEnd = look.offset + look.length;
                lookStart = lexer.position();
                lexer.next(look);
            }
        }
        Token next(){
            const Token token = look;
            skip();
            return token;
        }

        // where the parser is: set back to it to read the same tokens again (the lookahead is
        // read again from where it starts)
        struct State {
            size_t lookStart;
            uint32_t lastEnd;
            size_t errors;
        };
        State save() const { return State{lookStart, lastEnd, errors_.size()}; }
        void restorePosition(const State& state){
            lexer.reset(state.lookStart);
            lookStart = state.lookStart;
            lexer.next(look);
            lastEnd = state.lastEnd;
        }
        void restore(const State& state){
            restorePosition(state);
            errors_.resize(state.errors);
        }

        void discardWhitespace(){
            while (look.type == TokenType::Whitespace)
                skip();
        }
        uint32_t position() const { return look.offset; }

        void error(ParseError::Kind kind, uint32_t offset, uint32_t length){
            errors_.push_back(ParseError{kind, offset, length});
        }

        // reads the rest of the input (what the entry points that stop early leave): the
        // tokenizer's errors are those of the whole input
        void drain(){
            while (look.type != TokenType::EndOfFile)
                lexer.next(look);
        }

        // the tokenizer's bad tokens, then its other errors, then the parser's (in that order)
        std::vector<ParseError> allErrors(){
            drain();
            std::vector<ParseError> all;
            all.reserve(lexer.bad.size() + lexer.errors.size() + errors_.size());
            for (const CssLexer::BadToken& bad : lexer.bad)
                all.push_back(ParseError{bad.type == TokenType::BadString ? ParseError::Kind::BadString : ParseError::Kind::BadUrl, bad.offset, bad.length});
            for (const TokenizeError& error : lexer.errors){
                ParseError::Kind kind = ParseError::Kind::InvalidEscape;
                switch (error.kind){
                    case TokenizeError::Kind::EofInString: kind = ParseError::Kind::EofInString; break;
                    case TokenizeError::Kind::EofInUrl: kind = ParseError::Kind::EofInUrl; break;
                    case TokenizeError::Kind::EofInComment: kind = ParseError::Kind::EofInComment; break;
                    case TokenizeError::Kind::EofInEscape:
                    case TokenizeError::Kind::InvalidEscape: kind = ParseError::Kind::InvalidEscape; break;
                }
                all.push_back(ParseError{kind, error.offset, 0});
            }
            all.insert(all.end(), errors_.begin(), errors_.end());
            return all;
        }

        // ---- component values (§5.5.8-5.5.10), pushed on the value stack

        ValueList takeValues(size_t mark){ return takeFrom(scratch.values, mark); }

        // the token at the position as a component value
        void tokenValue(){
            ComponentValue& value = scratch.values.emplace_back();
            value.token = look;
            value.offset = look.offset;
            value.length = look.length;
            if (look.type == TokenType::RightParen || look.type == TokenType::RightBracket)
                error(ParseError::Kind::UnmatchedCloser, look.offset, look.length);
            skip();
        }

        void consumeComponentValue(){
            const TokenType type = look.type;
            if (type == TokenType::LeftBrace || type == TokenType::LeftBracket || type == TokenType::LeftParen)
                consumeBlockOrFunction(ComponentValue::Kind::Block, mirrorOf(type), ParseError::Kind::UnclosedBlock);
            else if (type == TokenType::Function)
                consumeBlockOrFunction(ComponentValue::Kind::Function, TokenType::RightParen, ParseError::Kind::UnclosedFunction);
            else
                tokenValue();
        }

        // a simple block or a function: its children built on the stack, then the value itself pushed
        void consumeBlockOrFunction(ComponentValue::Kind kind, TokenType ending, ParseError::Kind unclosed){
            ComponentValue value;
            value.kind = kind;
            value.token = look;
            value.offset = look.offset;
            skip();
            const size_t mark = scratch.values.size();
            while (true){
                const TokenType type = look.type;
                if (type == ending){
                    value.length = look.offset + look.length - value.offset;
                    skip();
                    break;
                }
                if (type == TokenType::EndOfFile){
                    value.closed = false;
                    error(unclosed, value.token.offset, value.token.length);
                    value.length = (scratch.values.size() == mark ? value.token.offset + value.token.length : endOf(scratch.values.back())) -
                                   value.offset;
                    break;
                }
                consumeComponentValue();
            }
            value.children = takeValues(mark);
            scratch.values.push_back(std::move(value));
        }

        // §5.5.7; stop not consumed
        void consumeComponentValues(bool stopAtSemicolon, bool nested){
            while (true){
                const TokenType type = look.type;
                if (type == TokenType::EndOfFile)
                    return;
                if (stopAtSemicolon && type == TokenType::Semicolon)
                    return;
                if (type == TokenType::RightBrace){
                    if (nested)
                        return;
                    error(ParseError::Kind::UnmatchedCloser, look.offset, look.length);
                    tokenValue();
                    continue;
                }
                consumeComponentValue();
            }
        }

        // ---- rules (§5.5.1-5.5.6)

        SourceRef ref(uint32_t begin, uint32_t end) const {
            return SourceRef{file, begin, end > begin ? end - begin : 0};
        }

        SourceRef preludeExtent(const ValueList& prelude) const {
            size_t begin = 0, end = prelude.size();
            while (begin < end && prelude[begin].isWhitespace())
                begin++;
            while (end > begin && prelude[end - 1].isWhitespace())
                end--;
            if (begin == end)
                return SourceRef{};
            const uint32_t from = prelude[begin].offset;
            const uint32_t to = endOf(prelude[end - 1]);
            return SourceRef{file, from, to > from ? to - from : 0};
        }

        // at the '{': the raw block, then its contents parsed
        void consumeBlock(Rule& rule){
            rule.hasBlock = true;
            if (!keepRawBlocks){
                // the contents pass alone: it ends on the same closing brace
                const Token opening = next();
                consumeBlockContentsInto(rule.declarations, rule.rules);
                uint32_t end = opening.offset + opening.length;
                if (look.type == TokenType::RightBrace){
                    end = look.offset + look.length;
                    skip();
                }else{
                    error(ParseError::Kind::UnclosedBlock, opening.offset, opening.length);
                    // (the last token read: the opening brace when the block is empty)
                    end = std::max(end, lastEnd);
                }
                rule.blockRef = ref(opening.offset, end);
                return;
            }
            const State open = save();
            const size_t errorsBefore = errors_.size();
            consumeBlockOrFunction(ComponentValue::Kind::Block, TokenType::RightBrace, ParseError::Kind::UnclosedBlock);
            ComponentValue raw = std::move(scratch.values.back());
            scratch.values.pop_back();
            const State afterRaw = save();
            // the contents pass reports its own errors for the same tokens
            errors_.resize(errorsBefore);

            restorePosition(open);
            skip();
            consumeBlockContentsInto(rule.declarations, rule.rules);
            if (look.type == TokenType::RightBrace)
                skip();
            else
                error(ParseError::Kind::UnclosedBlock, raw.token.offset, raw.token.length);
            // both passes end on the same closing brace
            restorePosition(afterRaw);

            rule.block = std::move(raw.children);
            rule.blockRef = ref(raw.offset, raw.offset + raw.length);
        }

        std::optional<Rule> consumeAtRule(bool nested){
            Rule rule;
            if (!consumeAtRuleInto(rule, nested))
                return std::nullopt;
            return rule;
        }

        bool consumeAtRuleInto(Rule& rule, bool nested){
            rule.kind = Rule::Kind::At;
            const Token keyword = next();
            rule.atName = toLowerAscii(std::string(keyword.value.view()));
            const uint32_t begin = keyword.offset;
            uint32_t end = keyword.offset + keyword.length;
            const size_t mark = scratch.values.size();
            bool taken = false;

            while (true){
                const TokenType type = look.type;
                if (type == TokenType::Semicolon){
                    end = look.offset + look.length;
                    skip();
                    break;
                }
                if (type == TokenType::EndOfFile)
                    break;
                if (type == TokenType::RightBrace){
                    if (nested)
                        break;
                    error(ParseError::Kind::UnmatchedCloser, look.offset, look.length);
                    tokenValue();
                    end = endOf(scratch.values.back());
                    continue;
                }
                if (type == TokenType::LeftBrace){
                    rule.prelude = takeValues(mark);
                    taken = true;
                    consumeBlock(rule);
                    end = rule.blockRef.offset + rule.blockRef.length;
                    break;
                }
                consumeComponentValue();
                end = endOf(scratch.values.back());
            }
            if (!taken)
                rule.prelude = takeValues(mark);

            rule.preludeRef = preludeExtent(rule.prelude);
            if (!rule.preludeRef.valid())
                rule.preludeRef = ref(keyword.offset + keyword.length, keyword.offset + keyword.length);
            else
                rule.preludeText = Text::view(std::string_view(sourceCopy + (rule.preludeRef.offset - base), rule.preludeRef.length));
            rule.ref = ref(begin, end);
            return true;
        }

        static bool looksLikeCustomProperty(const ComponentValue* first, const ComponentValue* last){
            size_t seen = 0;
            const ComponentValue* name = nullptr;
            for (const ComponentValue* value = first; value != last; ++value){
                if (value->isWhitespace())
                    continue;
                if (seen == 0){
                    name = value;
                }else{
                    return name->isToken(TokenType::Ident) && name->token.value.size() >= 2 &&
                           name->token.value[0] == '-' && name->token.value[1] == '-' && value->isToken(TokenType::Colon);
                }
                seen++;
            }
            return false;
        }

        std::optional<Rule> consumeQualifiedRule(bool stopAtSemicolon, bool nested){
            Rule rule;
            if (!consumeQualifiedRuleInto(rule, stopAtSemicolon, nested))
                return std::nullopt;
            return rule;
        }

        bool consumeQualifiedRuleInto(Rule& rule, bool stopAtSemicolon, bool nested){
            rule.kind = Rule::Kind::Style;
            const uint32_t begin = position();
            const size_t mark = scratch.values.size();
            if (deferPlainPreludes && deferPrelude(rule))
                return true;

            while (true){
                const TokenType type = look.type;
                if (type == TokenType::EndOfFile || (stopAtSemicolon && type == TokenType::Semicolon)){
                    error(ParseError::Kind::InvalidRule, begin, (look.offset > begin ? look.offset - begin : 0));
                    scratch.values.resize(mark);
                    return false;
                }
                if (type == TokenType::RightBrace){
                    if (nested){
                        error(ParseError::Kind::InvalidRule, begin, (look.offset > begin ? look.offset - begin : 0));
                        scratch.values.resize(mark);
                        return false;
                    }
                    error(ParseError::Kind::UnmatchedCloser, look.offset, look.length);
                    tokenValue();
                    continue;
                }
                if (type == TokenType::LeftBrace){
                    if (looksLikeCustomProperty(scratch.values.data() + mark, scratch.values.data() + scratch.values.size())){
                        scratch.values.resize(mark);
                        const uint32_t errorBegin = begin;
                        if (nested){
                            consumeBadDeclarationRemnants(true);
                        }else{
                            Rule dropped;
                            consumeBlock(dropped);
                        }
                        error(ParseError::Kind::InvalidRule, errorBegin, position() > errorBegin ? position() - errorBegin : 0);
                        return false;
                    }
                    rule.prelude = takeValues(mark);
                    consumeBlock(rule);
                    rule.preludeRef = preludeExtent(rule.prelude);
                    if (!rule.preludeRef.valid())
                        rule.preludeRef = ref(rule.blockRef.offset, rule.blockRef.offset);
                    else
                        rule.preludeText = Text::view(std::string_view(sourceCopy + (rule.preludeRef.offset - base), rule.preludeRef.length));
                    rule.ref = ref(begin, rule.blockRef.offset + rule.blockRef.length);
                    return true;
                }
                consumeComponentValue();
            }
        }

        // A plain prelude (ASCII name characters, '.', '#', ':', '>', '+', '~', '*', ',' and white
        // space up to a '{'): read as text without its tokens (they hold no error and no block),
        // then the block. false: not plain (nothing consumed).
        bool deferPrelude(Rule& rule){
            const size_t start = lookStart;
            const size_t size = css.size();
            // (a comment before the lookahead: the token path reads it)
            if (look.offset != base + (uint32_t)start)
                return false;
            size_t i = start;
            // ("--x: ..." may be a custom property: the token path decides)
            if (i + 1 < size && css[i] == '-' && css[i + 1] == '-')
                return false;
            size_t lastNonSpace = start;
            while (i < size){
                const PreludeByte kind = kPreludeBytes.kind[(unsigned char)css[i]];
                if (kind == PreludeByte::Plain){
                    i++;
                    lastNonSpace = i;
                    continue;
                }
                if (kind == PreludeByte::Space){
                    i++;
                    continue;
                }
                if (kind == PreludeByte::Open)
                    break;
                return false;
            }
            if (i >= size || lastNonSpace == start)
                return false;
            rule.kind = Rule::Kind::Style;
            rule.preludeDeferred = true;
            const uint32_t begin = base + (uint32_t)start;
            rule.preludeRef = ref(begin, base + (uint32_t)lastNonSpace);
            rule.preludeText = Text::view(std::string_view(sourceCopy + start, lastNonSpace - start));
            // the '{' is the lookahead now
            lexer.reset(i);
            lookStart = i;
            lexer.next(look);
            lastEnd = base + (uint32_t)lastNonSpace;
            consumeBlock(rule);
            rule.ref = ref(begin, rule.blockRef.offset + rule.blockRef.length);
            return true;
        }

        void consumeBadDeclarationRemnants(bool nested){
            const size_t mark = scratch.values.size();
            while (true){
                const TokenType type = look.type;
                if (type == TokenType::EndOfFile)
                    break;
                if (type == TokenType::Semicolon){
                    skip();
                    break;
                }
                if (type == TokenType::RightBrace){
                    if (nested)
                        break;
                    skip();
                    continue;
                }
                consumeComponentValue();
            }
            scratch.values.resize(mark);
        }

        // §5.5.6
        std::optional<Declaration> consumeDeclaration(bool nested){
            Declaration declaration;
            if (consumeDeclarationInto(declaration, nested, true) != DeclarationRead::Parsed)
                return std::nullopt;
            return declaration;
        }

        enum class DeclarationRead : uint8_t {
            Parsed,
            NotADeclaration,    // no ident followed by a colon
            Invalid,            // its value holds a {} block next to other values: a rule, in a block
        };

        // remnants: a declaration without its name and colon is consumed to its end (outside a
        // block's contents, which read a rule from the same position instead)
        DeclarationRead consumeDeclarationInto(Declaration& declaration, bool nested, bool remnants){
            if (look.type != TokenType::Ident){
                if (remnants)
                    consumeBadDeclarationRemnants(nested);
                return DeclarationRead::NotADeclaration;
            }
            const uint32_t nameOffset = look.offset, nameLength = look.length;
            const Text nameValue = look.value;
            skip();
            const std::string_view nameText = nameValue.view();
            const bool custom = nameText.size() >= 2 && nameText[0] == '-' && nameText[1] == '-';
            {
                // the name as written when it is lower case already (a view of the text the
                // token's value is a view of), a lowered copy otherwise
                bool upper = false;
                for (const char c : nameText)
                    upper |= (unsigned char)(c - 'A') < 26u;
                if (custom || !upper)
                    declaration.name = nameValue;
                else
                    declaration.name = Text(toLowerAscii(std::string(nameText)));
            }
            declaration.nameRef = ref(nameOffset, nameOffset + nameLength);

            discardWhitespace();
            if (look.type != TokenType::Colon){
                if (remnants)
                    consumeBadDeclarationRemnants(nested);
                return DeclarationRead::NotADeclaration;
            }
            const uint32_t colonEnd = look.offset + look.length;
            skip();
            discardWhitespace();
            const uint32_t valueBegin = position() < colonEnd ? colonEnd : position();

            std::vector<ComponentValue>& values = scratch.values;
            const size_t mark = values.size();
            consumeComponentValues(true, nested);

            // trailing whitespace, then "! important"
            while (values.size() > mark && values.back().isWhitespace())
                values.pop_back();
            const uint32_t declarationEnd = values.size() == mark ? valueBegin : endOf(values.back());
            {
                const size_t last = values.size() - mark;
                const ComponentValue* v = values.data() + mark;
                if (last >= 1 && v[last - 1].isToken(TokenType::Ident) && v[last - 1].token.isIdent("important")){
                    size_t bang = last - 1;
                    while (bang > 0 && v[bang - 1].isWhitespace())
                        bang--;
                    if (bang > 0 && v[bang - 1].isToken(TokenType::Delim) && v[bang - 1].token.isDelim('!')){
                        declaration.important = true;
                        values.resize(mark + bang - 1);
                    }
                }
            }
            while (values.size() > mark && values.back().isWhitespace())
                values.pop_back();

            if (!custom){
                bool hasBraceBlock = false;
                bool hasOther = false;
                for (size_t i = mark; i < values.size(); i++){
                    if (values[i].isBlock(TokenType::LeftBrace))
                        hasBraceBlock = true;
                    else if (!values[i].isWhitespace())
                        hasOther = true;
                }
                if (hasBraceBlock && hasOther){
                    values.resize(mark);
                    return DeclarationRead::Invalid;
                }
            }

            if (!custom && declaration.name == "unicode-range" && values.size() > mark){
                // the unicode-range descriptor: its source text again, with <unicode-range-token>s
                const uint32_t from = values[mark].offset;
                const uint32_t to = endOf(values.back());
                values.resize(mark);
                TokenizeOptions options;
                options.unicodeRanges = true;
                declaration.value = parseComponentValueList(css.substr(from - base, to - from), from, options, nullptr);
            }else{
                declaration.value = takeValues(mark);
            }

            declaration.valueRef = declaration.value.empty() ? ref(valueBegin, valueBegin)
                                                             : ref(declaration.value.front().offset, endOf(declaration.value.back()));
            declaration.ref = ref(nameOffset, declarationEnd);
            declaration.text = Text::view(std::string_view(sourceCopy + (nameOffset - base), declarationEnd - nameOffset));
            return DeclarationRead::Parsed;
        }

        // §5.5.5, stops at '}' (not consumed) or EOF; the declarations and rules pushed on their stacks
        void consumeBlockContents(){
            while (true){
                const TokenType type = look.type;
                if (type == TokenType::Whitespace || type == TokenType::Semicolon){
                    skip();
                    continue;
                }
                if (type == TokenType::EndOfFile || type == TokenType::RightBrace)
                    return;
                if (type == TokenType::AtKeyword){
                    Rule rule;
                    if (consumeAtRuleInto(rule, true))
                        scratch.rules.push_back(std::move(rule));
                    continue;
                }

                // "consume a declaration", and when it returns nothing the rule from the same
                // position, read again
                bool declarationShaped = false;
                if (type == TokenType::Ident){
                    const State before = save();
                    // (built in place: reading a value pushes no declaration)
                    const DeclarationRead read = consumeDeclarationInto(scratch.declarations.emplace_back(), true, false);
                    if (read == DeclarationRead::Parsed)
                        continue;
                    scratch.declarations.pop_back();
                    // (an ident followed by a colon: reported as a bad declaration rather than a bad rule)
                    declarationShaped = read == DeclarationRead::Invalid;
                    restore(before);
                }

                Rule rule;
                if (consumeQualifiedRuleInto(rule, true, true))
                    scratch.rules.push_back(std::move(rule));
                else if (declarationShaped && !errors_.empty() && errors_.back().kind == ParseError::Kind::InvalidRule)
                    errors_.back().kind = ParseError::Kind::InvalidDeclaration;
            }
        }

        void consumeBlockContentsInto(DeclarationList& declarations, RuleList& rules){
            const size_t declarationMark = scratch.declarations.size();
            const size_t ruleMark = scratch.rules.size();
            consumeBlockContents();
            declarations = takeFrom(scratch.declarations, declarationMark);
            rules = takeFrom(scratch.rules, ruleMark);
        }

        // a declaration list (style="", parseBlockContents): block contents to the end, a stray
        // '}' ending nothing
        void consumeBlockContentsList(DeclarationList& declarations, RuleList& rules){
            const size_t declarationMark = scratch.declarations.size();
            const size_t ruleMark = scratch.rules.size();
            while (true){
                consumeBlockContents();
                if (atEnd())
                    break;
                const Token closer = next();
                error(ParseError::Kind::UnmatchedCloser, closer.offset, closer.length);
            }
            declarations = takeFrom(scratch.declarations, declarationMark);
            rules = takeFrom(scratch.rules, ruleMark);
        }

        ValueList consumeValueList(){
            const size_t mark = scratch.values.size();
            consumeComponentValues(false, false);
            return takeValues(mark);
        }

        ComponentValue consumeOneValue(){
            consumeComponentValue();
            ComponentValue value = std::move(scratch.values.back());
            scratch.values.pop_back();
            return value;
        }

        // §5.5.1 (topLevel) or the 2021 draft's "consume a list of rules"
        RuleList consumeRuleList(bool topLevel){
            const size_t mark = scratch.rules.size();
            while (true){
                const TokenType type = look.type;
                if (type == TokenType::Whitespace){
                    skip();
                    continue;
                }
                if (type == TokenType::EndOfFile)
                    break;
                if (topLevel && (type == TokenType::CDO || type == TokenType::CDC)){
                    skip();
                    continue;
                }
                Rule rule;
                const bool parsed = type == TokenType::AtKeyword ? consumeAtRuleInto(rule, false) : consumeQualifiedRuleInto(rule, false, false);
                if (parsed)
                    scratch.rules.push_back(std::move(rule));
            }
            return takeFrom(scratch.rules, mark);
        }

        void report(DiagnosticSink* sink){
            if (!sink)
                return;
            const std::vector<ParseError> all = allErrors();
            // a bad string or url already explains the declaration or rule it breaks
            std::vector<uint32_t> badTokens;
            for (const ParseError& error : all){
                if (error.kind == ParseError::Kind::BadString || error.kind == ParseError::Kind::BadUrl)
                    badTokens.push_back(error.offset);
            }
            for (const ParseError& error : all){
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
        const char* sourceCopy;
        CssLexer lexer;
        Token look;                  // the next token
        size_t lookStart = 0;        // where the lexer started reading it
        uint32_t lastEnd = 0;        // the end of the last token read
        // Rule::block (the raw block) is kept for the CSS Syntax entry points; the
        // compatibility layer's parseStylesheet/parseDeclarationList only use the contents
        bool keepRawBlocks = true;
        // parseStylesheet(..., deferPlainPreludes)
        bool deferPlainPreludes = false;
        std::vector<ParseError> errors_;
        std::vector<ParseError>* errorsOut;
        Scratch& scratch;
        const size_t valuesMark, declarationsMark, rulesMark;
    };

}

SourceRef jasperengine::htmlui::compat::extentOf(uint32_t file, const ValueList& values){
    if (values.empty())
        return SourceRef{};
    const uint32_t begin = values.front().offset;
    const uint32_t end = endOf(values.back());
    return SourceRef{file, begin, end > begin ? end - begin : 0};
}

ValueList jasperengine::htmlui::compat::trimWhitespace(const ValueList& values){
    size_t begin = 0;
    size_t end = values.size();
    while (begin < end && values[begin].isWhitespace())
        begin++;
    while (end > begin && values[end - 1].isWhitespace())
        end--;
    return ValueList(values.begin() + (std::ptrdiff_t)begin, values.begin() + (std::ptrdiff_t)end);
}

std::vector<ValueList> jasperengine::htmlui::compat::splitAtCommas(const ValueList& values){
    // each group copied once, from its range
    std::vector<ValueList> groups;
    size_t commas = 0;
    for (const ComponentValue& value : values)
        commas += value.isToken(TokenType::Comma) ? 1 : 0;
    groups.reserve(commas + 1);
    size_t begin = 0;
    for (size_t i = 0; i <= values.size(); i++){
        if (i < values.size() && !values[i].isToken(TokenType::Comma))
            continue;
        groups.emplace_back(values.begin() + (std::ptrdiff_t)begin, values.begin() + (std::ptrdiff_t)i);
        begin = i + 1;
    }
    return groups;
}

Stylesheet jasperengine::htmlui::compat::parseStylesheet(uint32_t file, std::string_view css, DiagnosticSink& sink, uint32_t baseOffset,
                                                       bool deferPlainPreludes){
    Stylesheet sheet;
    sheet.file = file;
    Parser parser(file, css, baseOffset, TokenizeOptions(), nullptr);
    parser.keepRawBlocks = false;
    parser.deferPlainPreludes = deferPlainPreludes;
    sheet.rules = parser.consumeRuleList(true);
    parser.report(&sink);
    return sheet;
}

ValueList jasperengine::htmlui::compat::preludeOf(const Rule& rule){
    if (!rule.preludeDeferred)
        return rule.prelude;
    // the prelude's text and the white space after it, up to the block (in the parser's copy of
    // the source, as the prelude text): the values the token path reads
    const std::string_view text(rule.preludeText.data(), rule.blockRef.offset - rule.preludeRef.offset);
    return parseComponentValueList(text, rule.preludeRef.offset);
}

DeclarationList jasperengine::htmlui::compat::parseDeclarationList(uint32_t file, std::string_view css, uint32_t baseOffset,
                                                                            DiagnosticSink& sink){
    Parser parser(file, css, baseOffset, TokenizeOptions(), nullptr);
    parser.keepRawBlocks = false;
    DeclarationList declarations;
    RuleList rules;
    parser.consumeBlockContentsList(declarations, rules);
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
    parser.consumeBlockContentsList(result.declarations, result.rules);
    result.ref = SourceRef{file, baseOffset, (uint32_t)css.size()};
    parser.report(sink);
    return result;
}

RuleList jasperengine::htmlui::compat::parseRuleList(uint32_t file, std::string_view css, uint32_t baseOffset, bool topLevel,
                                                              DiagnosticSink* sink, std::vector<ParseError>* errors){
    Parser parser(file, css, baseOffset, TokenizeOptions(), errors);
    RuleList rules = parser.consumeRuleList(topLevel);
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

ValueList jasperengine::htmlui::compat::parseComponentValueList(std::string_view css, uint32_t baseOffset,
                                                                                  const TokenizeOptions& options,
                                                                                  std::vector<ParseError>* errors){
    Parser parser(0, css, baseOffset, options, errors);
    return parser.consumeValueList();
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
    ComponentValue value = parser.consumeOneValue();
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
