// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_TEST_COMMON_H
#define COMPAT_TEST_COMMON_H

// Shared helpers of the compat tests (engine/tests/htmlui/compat): fixture paths and the
// css-parsing-tests JSON representation of component values, rules and declarations
// (fixtures/compat/css-parsing-tests/README.rst, "Result representation").

#include "htmlui/compat/css/CssAst.h"
#include "htmlui/compat/css/CssParser.h"

#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace jasperengine::test::compat {

    using namespace jasperengine::htmlui::compat;

    inline std::string fixturePath(const std::string& relative){
        return std::string(JASPER_HTMLUI_TEST_SOURCE_DIR) + "/fixtures/" + relative;
    }

    inline bool readFixture(const std::string& relative, std::string& content){
        std::ifstream file(fixturePath(relative), std::ios::binary);
        if (!file)
            return false;
        std::ostringstream stream;
        stream << file.rdbuf();
        content = stream.str();
        return true;
    }

    // one css-parsing-tests file: pairs of (input, expected)
    inline nlohmann::json loadParsingTests(const std::string& name){
        std::string text;
        if (!readFixture("compat/css-parsing-tests/" + name, text))
            return nlohmann::json();
        return nlohmann::json::parse(text);
    }

    // The characters of a numeric token's number (its representation in the tests): the
    // token text without the unit or '%'
    inline std::string numberRepresentation(std::string_view source, const Token& token, uint32_t baseOffset){
        const std::string_view text = source.substr(token.offset - baseOffset, token.length);
        size_t i = 0;
        auto digit = [&](size_t at){ return at < text.size() && text[at] >= '0' && text[at] <= '9'; };
        if (i < text.size() && (text[i] == '+' || text[i] == '-'))
            i++;
        while (digit(i))
            i++;
        if (i < text.size() && text[i] == '.' && digit(i + 1)){
            i++;
            while (digit(i))
                i++;
        }
        if (i < text.size() && (text[i] == 'e' || text[i] == 'E')){
            if (digit(i + 1)){
                i += 1;
                while (digit(i))
                    i++;
            }else if (i + 2 < text.size() + 1 && (text[i + 1] == '+' || text[i + 1] == '-') && digit(i + 2)){
                i += 2;
                while (digit(i))
                    i++;
            }
        }
        return std::string(text.substr(0, i));
    }

    // Converts parsed values to the tests' JSON. errorsAtEnd: tokenizer errors that the tests
    // list right after the token ending at their offset (eof-in-string, eof-in-url).
    class ParsingTestsJson {
    public:
        ParsingTestsJson(std::string_view source, uint32_t baseOffset, std::vector<ParseError> errors = {}):
            source(source), baseOffset(baseOffset) {
            for (const ParseError& error : errors){
                if (error.kind == ParseError::Kind::EofInString || error.kind == ParseError::Kind::EofInUrl)
                    pending.push_back(error);
            }
        }

        nlohmann::json list(const ValueList& values){
            nlohmann::json result = nlohmann::json::array();
            for (size_t i = 0; i < values.size(); i++){
                const ComponentValue& value = values[i];
                // 2021 draft match tokens: two adjacent delims
                if (value.isToken(TokenType::Delim) && i + 1 < values.size() && values[i + 1].isToken(TokenType::Delim) &&
                    value.offset + value.length == values[i + 1].offset){
                    const std::string pair = value.token.value + values[i + 1].token.value;
                    if (pair == "~=" || pair == "|=" || pair == "^=" || pair == "$=" || pair == "*=" || pair == "||"){
                        result.push_back(pair);
                        i++;
                        appendErrorsEndingAt(result, values[i].offset + values[i].length);
                        continue;
                    }
                }
                result.push_back(one(value));
                appendErrorsEndingAt(result, value.offset + value.length);
            }
            return result;
        }

        nlohmann::json one(const ComponentValue& value){
            if (value.kind == ComponentValue::Kind::Function){
                nlohmann::json result = nlohmann::json::array({"function", value.token.value});
                for (nlohmann::json& child : list(value.children))
                    result.push_back(child);
                return result;
            }
            if (value.kind == ComponentValue::Kind::Block){
                const char* name = value.token.type == TokenType::LeftBrace ? "{}" : value.token.type == TokenType::LeftBracket ? "[]" : "()";
                nlohmann::json result = nlohmann::json::array({name});
                for (nlohmann::json& child : list(value.children))
                    result.push_back(child);
                return result;
            }
            return token(value.token);
        }

        nlohmann::json token(const Token& t){
            const auto numberType = [&](){ return t.isInteger ? "integer" : "number"; };
            switch (t.type){
                case TokenType::Ident: return nlohmann::json::array({"ident", t.value});
                case TokenType::AtKeyword: return nlohmann::json::array({"at-keyword", t.value});
                case TokenType::Hash: return nlohmann::json::array({"hash", t.value, t.hashIsId ? "id" : "unrestricted"});
                case TokenType::String: return nlohmann::json::array({"string", t.value});
                case TokenType::BadString: return nlohmann::json::array({"error", "bad-string"});
                case TokenType::Url: return nlohmann::json::array({"url", t.value});
                case TokenType::BadUrl: return nlohmann::json::array({"error", "bad-url"});
                case TokenType::Delim: return t.value;
                case TokenType::Number:
                    return nlohmann::json::array({"number", numberRepresentation(source, t, baseOffset), t.number, numberType()});
                case TokenType::Percentage:
                    return nlohmann::json::array({"percentage", numberRepresentation(source, t, baseOffset), t.number, numberType()});
                case TokenType::Dimension:
                    return nlohmann::json::array({"dimension", numberRepresentation(source, t, baseOffset), t.number, numberType(), t.unit});
                case TokenType::UnicodeRange: return nlohmann::json::array({"unicode-range", t.rangeStart, t.rangeEnd});
                case TokenType::Whitespace: return " ";
                case TokenType::CDO: return "<!--";
                case TokenType::CDC: return "-->";
                case TokenType::Colon: return ":";
                case TokenType::Semicolon: return ";";
                case TokenType::Comma: return ",";
                case TokenType::RightParen: return nlohmann::json::array({"error", ")"});
                case TokenType::RightBracket: return nlohmann::json::array({"error", "]"});
                case TokenType::RightBrace: return nlohmann::json::array({"error", "}"});
                default: return nlohmann::json::array({"unexpected", tokenTypeName(t.type)});
            }
        }

        nlohmann::json declaration(const Declaration& d){
            return nlohmann::json::array({"declaration", d.name, list(d.value), d.important});
        }

        nlohmann::json rule(const Rule& r){
            if (r.kind == Rule::Kind::At){
                return nlohmann::json::array({"at-rule", r.atName, list(r.prelude),
                                              r.hasBlock ? list(r.block) : nlohmann::json(nullptr)});
            }
            return nlohmann::json::array({"qualified rule", list(r.prelude), list(r.block)});
        }

    private:
        void appendErrorsEndingAt(nlohmann::json& out, uint32_t end){
            for (auto it = pending.begin(); it != pending.end();){
                if (it->offset == end){
                    out.push_back(nlohmann::json::array({"error", parseErrorName(it->kind)}));
                    it = pending.erase(it);
                }else{
                    ++it;
                }
            }
        }

        std::string_view source;
        uint32_t baseOffset;
        std::vector<ParseError> pending;
    };

    // rules (or declarations and rules) and the "invalid" / "empty" / "extra-input" errors,
    // in source order, as the tests list them
    inline nlohmann::json itemsWithErrors(ParsingTestsJson& json, const std::vector<Declaration>& declarations,
                                          const std::vector<Rule>& rules, const std::vector<ParseError>& errors){
        std::vector<std::pair<uint32_t, nlohmann::json>> items;
        for (const Declaration& declaration : declarations)
            items.emplace_back(declaration.ref.offset, json.declaration(declaration));
        for (const Rule& rule : rules)
            items.emplace_back(rule.ref.offset, json.rule(rule));
        for (const ParseError& error : errors){
            // errors inside a returned rule's block are not items of this list (the 2021
            // tests keep blocks raw)
            bool nested = false;
            for (const Rule& rule : rules){
                if (rule.hasBlock && error.offset > rule.blockRef.offset && error.offset < rule.blockRef.offset + rule.blockRef.length)
                    nested = true;
            }
            if (nested)
                continue;
            if (error.kind == ParseError::Kind::InvalidRule || error.kind == ParseError::Kind::InvalidDeclaration)
                items.emplace_back(error.offset, nlohmann::json::array({"error", "invalid"}));
            else if (error.kind == ParseError::Kind::Empty || error.kind == ParseError::Kind::ExtraInput)
                items.emplace_back(error.offset, nlohmann::json::array({"error", parseErrorName(error.kind)}));
        }
        std::stable_sort(items.begin(), items.end(), [](const auto& a, const auto& b){ return a.first < b.first; });
        nlohmann::json result = nlohmann::json::array();
        for (auto& item : items)
            result.push_back(std::move(item.second));
        return result;
    }

    // JSON equality with a relative tolerance on numbers (the tests' values are decimal)
    inline bool jsonNear(const nlohmann::json& a, const nlohmann::json& b){
        if (a.is_number() && b.is_number()){
            const double x = a.get<double>();
            const double y = b.get<double>();
            return std::fabs(x - y) <= 1e-9 * std::max(1.0, std::max(std::fabs(x), std::fabs(y)));
        }
        if (a.is_array() && b.is_array()){
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); i++){
                if (!jsonNear(a[i], b[i]))
                    return false;
            }
            return true;
        }
        return a == b;
    }

    // a declaration value as the 2021 tests write it, without leading/trailing whitespace
    inline nlohmann::json trimJsonWhitespace(const nlohmann::json& values){
        nlohmann::json result = values;
        while (!result.empty() && result.front() == " ")
            result.erase(result.begin());
        while (!result.empty() && result.back() == " ")
            result.erase(result.end() - 1);
        return result;
    }

}

#endif //COMPAT_TEST_COMMON_H
