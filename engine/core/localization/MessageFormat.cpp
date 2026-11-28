// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MessageFormat.h"

#include "localization/PluralRules.h"

#include <cmath>
#include <cstdlib>

using namespace jasperengine;

namespace jasperengine::l10n {

namespace {

    constexpr int kMaxDepth = 4;

    struct PluralNumber {
        bool present = false;
        double value = 0;           // the argument minus the offset
        bool integer = false;
        int64_t integerValue = 0;
    };

    bool isNameChar(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    }

    bool isSpace(char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    }

    const LocalizationValue* findArg(const LocalizationArgs* args, std::string_view name) {
        if (!args) return nullptr;
        for (const auto& entry : *args) {
            if (entry.first == name) return &entry.second;
        }
        return nullptr;
    }

    // a number argument, or a string that reads as one
    bool numberOf(const LocalizationValue& value, double& number, bool& integer, int64_t& integerValue) {
        if (const int64_t* i = std::get_if<int64_t>(&value)) {
            integer = true;
            integerValue = *i;
            number = static_cast<double>(*i);
            return true;
        }
        if (const double* d = std::get_if<double>(&value)) {
            integer = false;
            number = *d;
            return true;
        }
        const std::string& text = std::get<std::string>(value);
        if (text.empty()) return false;
        char* end = nullptr;
        number = std::strtod(text.c_str(), &end);
        if (end != text.c_str() + text.size() || !std::isfinite(number)) return false;
        integer = false;
        return true;
    }

    std::string formatPlain(double number, bool integer, int64_t integerValue, std::string_view language, NumberStyle style) {
        if (integer && style != NumberStyle::Percent) return PluralRules::formatInteger(integerValue, language);
        return PluralRules::formatNumber(number, language, style);
    }

    struct Parser {
        std::string_view p;
        size_t pos = 0;
        const FormatOptions& options;
        MessageError error;
        bool failed = false;
        std::string literal;        // the literal run being collected when a transform is on

        Parser(std::string_view pattern, const FormatOptions& o) : p(pattern), options(o) {}

        bool fail(const char* message) {
            if (!failed) {
                failed = true;
                error.offset = pos < p.size() ? pos : p.size();
                error.message = message;
            }
            return false;
        }

        void skipSpace() {
            while (pos < p.size() && isSpace(p[pos])) pos++;
        }

        std::string_view name() {
            const size_t start = pos;
            while (pos < p.size() && isNameChar(p[pos])) pos++;
            return p.substr(start, pos - start);
        }

        void appendLiteral(std::string* out, std::string_view text) {
            if (!out) return;
            if (options.literal) literal.append(text);
            else out->append(text);
        }

        void flush(std::string* out) {
            if (!out || literal.empty()) return;
            options.literal(literal, *out);
            literal.clear();
        }

        // text up to the closing '}' of a nested message (not consumed) or the end
        bool message(std::string* out, int depth, const PluralNumber& plural, bool nested) {
            while (pos < p.size()) {
                const char c = p[pos];
                if (c == '\'') {
                    const char next = pos + 1 < p.size() ? p[pos + 1] : '\0';
                    if (next == '\'') {
                        appendLiteral(out, "'");
                        pos += 2;
                    } else if (next == '{' || next == '}' || next == '|' || (next == '#' && plural.present)) {
                        pos++;
                        while (pos < p.size()) {
                            if (p[pos] == '\'') {
                                if (pos + 1 < p.size() && p[pos + 1] == '\'') {
                                    appendLiteral(out, "'");
                                    pos += 2;
                                    continue;
                                }
                                pos++;
                                break;
                            }
                            appendLiteral(out, p.substr(pos, 1));
                            pos++;
                        }
                    } else {
                        appendLiteral(out, "'");
                        pos++;
                    }
                } else if (c == '{') {
                    flush(out);
                    if (!argument(out, depth, plural)) return false;
                } else if (c == '}') {
                    if (!nested) return fail("a } closes nothing");
                    flush(out);
                    return true;
                } else if (c == '#' && plural.present) {
                    flush(out);
                    if (out) out->append(formatPlain(plural.value, plural.integer, plural.integerValue, options.language, NumberStyle::Decimal));
                    pos++;
                } else {
                    const size_t start = pos;
                    while (pos < p.size() && p[pos] != '\'' && p[pos] != '{' && p[pos] != '}' && !(p[pos] == '#' && plural.present)) pos++;
                    appendLiteral(out, p.substr(start, pos - start));
                }
            }
            if (nested) return fail("a { is not closed");
            flush(out);
            return true;
        }

        void missing(std::string* out, std::string_view argName) {
            if (!out || !options.showMissing) return;
            out->append("{");
            out->append(argName);
            out->append("}");
        }

        void simple(std::string* out, std::string_view argName) {
            if (!out) return;
            const LocalizationValue* value = findArg(options.args, argName);
            if (!value) return missing(out, argName);
            if (const std::string* text = std::get_if<std::string>(value)) {
                if (text->size() > 1 && (*text)[0] == '@' && options.resolveKey) {
                    std::string referenced;
                    std::string language;
                    if (options.resolveKey(options.resolveContext, std::string_view(*text).substr(1), referenced, language)) {
                        FormatOptions inner = options;
                        inner.resolveKey = nullptr;         // one level
                        inner.literal = nullptr;            // the resolver already gave the shown text
                        if (!language.empty()) inner.language = language;
                        MessageFormat::format(referenced, inner, *out);
                        return;
                    }
                }
                out->append(*text);
                return;
            }
            double number = 0;
            bool integer = false;
            int64_t integerValue = 0;
            numberOf(*value, number, integer, integerValue);
            out->append(formatPlain(number, integer, integerValue, options.language, NumberStyle::Decimal));
        }

        bool argument(std::string* out, int depth, const PluralNumber& plural) {
            pos++;      // '{'
            skipSpace();
            const std::string_view argName = name();
            if (argName.empty()) return fail("an argument needs a name: {name}");
            skipSpace();
            if (pos >= p.size()) return fail("a { is not closed");
            if (p[pos] == '}') {
                pos++;
                simple(out, argName);
                return true;
            }
            if (p[pos] != ',') return fail("expected , or } after the argument name");
            pos++;
            skipSpace();
            const std::string_view type = name();
            skipSpace();
            if (type == "number") return number(out, argName);
            if (type == "plural" || type == "select") {
                if (depth + 1 > kMaxDepth) return fail("plural and select nest at most 4 levels");
                return choice(out, argName, type == "plural", depth + 1, plural);
            }
            if (type.empty()) return fail("expected an argument type: number, plural or select");
            return fail("unknown argument type (number, plural or select)");
        }

        bool number(std::string* out, std::string_view argName) {
            NumberStyle style = NumberStyle::Decimal;
            if (pos < p.size() && p[pos] == ',') {
                pos++;
                skipSpace();
                const std::string_view styleName = name();
                skipSpace();
                if (styleName == "integer") style = NumberStyle::Integer;
                else if (styleName == "percent") style = NumberStyle::Percent;
                else return fail("unknown number style (integer or percent)");
            }
            if (pos >= p.size() || p[pos] != '}') return fail("expected } after the number argument");
            pos++;
            if (!out) return true;
            const LocalizationValue* value = findArg(options.args, argName);
            if (!value) {
                missing(out, argName);
                return true;
            }
            double numberValue = 0;
            bool integer = false;
            int64_t integerValue = 0;
            if (!numberOf(*value, numberValue, integer, integerValue)) {
                if (const std::string* text = std::get_if<std::string>(value)) out->append(*text);
                return true;
            }
            out->append(formatPlain(numberValue, integer, integerValue, options.language, style));
            return true;
        }

        struct Case {
            std::string_view selector;
            size_t start = 0;
        };

        bool choice(std::string* out, std::string_view argName, bool isPlural, int depth, const PluralNumber& outer) {
            if (pos >= p.size() || p[pos] != ',') return fail(isPlural ? "expected , after plural" : "expected , after select");
            pos++;
            skipSpace();
            double offset = 0;
            if (isPlural && p.substr(pos, 7) == "offset:") {
                pos += 7;
                skipSpace();
                const size_t start = pos;
                while (pos < p.size() && p[pos] >= '0' && p[pos] <= '9') pos++;
                if (pos == start || pos - start > 9) return fail("offset: needs a whole number");
                offset = std::strtod(std::string(p.substr(start, pos - start)).c_str(), nullptr);
            }
            // every case once without output: the syntax, and where each message starts
            Case cases[64];
            size_t caseCount = 0;
            bool hasOther = false;
            PluralNumber skipPlural;
            skipPlural.present = isPlural || outer.present;
            for (;;) {
                skipSpace();
                if (pos >= p.size()) return fail("a { is not closed");
                if (p[pos] == '}') break;
                const size_t selectorStart = pos;
                if (isPlural && p[pos] == '=') {
                    pos++;
                    const size_t digits = pos;
                    if (pos < p.size() && p[pos] == '-') pos++;
                    while (pos < p.size() && ((p[pos] >= '0' && p[pos] <= '9') || p[pos] == '.')) pos++;
                    if (pos == digits) return fail("=N needs a number");
                } else {
                    while (pos < p.size() && (isNameChar(p[pos]) || p[pos] == '-')) pos++;
                    if (pos == selectorStart) return fail("expected a case: a keyword or =N, then {message}");
                }
                const std::string_view selector = p.substr(selectorStart, pos - selectorStart);
                if (isPlural && selector[0] != '=') {
                    PluralCategory category;
                    if (!PluralRules::fromName(selector, category)) return fail("plural cases are zero, one, two, few, many, other or =N");
                }
                if (selector == "other") hasOther = true;
                skipSpace();
                if (pos >= p.size() || p[pos] != '{') return fail("expected { after the case");
                pos++;
                if (caseCount == 64) return fail("more than 64 cases");
                cases[caseCount++] = {selector, pos};
                if (!message(nullptr, depth, skipPlural, true)) return false;
                pos++;      // '}'
            }
            if (!hasOther) return fail(isPlural ? "a plural needs an other case" : "a select needs an other case");
            const size_t end = pos + 1;
            pos = end;
            if (!out) return true;

            const LocalizationValue* value = findArg(options.args, argName);
            if (!value) {
                missing(out, argName);
                return true;
            }
            const Case* chosen = nullptr;
            PluralNumber inner = outer;
            if (isPlural) {
                double number = 0;
                bool integer = false;
                int64_t integerValue = 0;
                const bool isNumber = numberOf(*value, number, integer, integerValue);
                inner.present = true;
                inner.value = number - offset;
                inner.integer = integer;
                inner.integerValue = integerValue - static_cast<int64_t>(offset);
                if (isNumber) {
                    for (size_t k = 0; k < caseCount && !chosen; k++) {
                        if (cases[k].selector[0] == '=' &&
                                std::strtod(std::string(cases[k].selector.substr(1)).c_str(), nullptr) == number) chosen = &cases[k];
                    }
                    if (!chosen) {
                        const PluralCategory category = PluralRules::select(options.language, inner.value, NumberStyle::Decimal);
                        const char* keyword = PluralRules::name(category);
                        for (size_t k = 0; k < caseCount && !chosen; k++) {
                            if (cases[k].selector == keyword) chosen = &cases[k];
                        }
                    }
                }
            } else {
                std::string key;
                if (const std::string* text = std::get_if<std::string>(value)) key = *text;
                else if (const int64_t* i = std::get_if<int64_t>(value)) key = std::to_string(*i);
                for (size_t k = 0; k < caseCount && !chosen; k++) {
                    if (cases[k].selector == key) chosen = &cases[k];
                }
            }
            if (!chosen) {
                for (size_t k = 0; k < caseCount && !chosen; k++) {
                    if (cases[k].selector == "other") chosen = &cases[k];
                }
            }
            pos = chosen->start;
            message(out, depth, inner, true);
            pos = end;
            return true;
        }
    };

}

bool MessageFormat::isPlain(std::string_view pattern) {
    return pattern.find_first_of("{}'") == std::string_view::npos;
}

bool MessageFormat::validate(std::string_view pattern, MessageError* error) {
    FormatOptions options;
    Parser parser(pattern, options);
    const bool ok = parser.message(nullptr, 0, PluralNumber(), false);
    if (!ok && error) *error = parser.error;
    return ok;
}

bool MessageFormat::format(std::string_view pattern, const FormatOptions& options, std::string& out) {
    if (isPlain(pattern)) {
        if (options.literal) options.literal(pattern, out);
        else out.append(pattern);
        return true;
    }
    // the syntax first, so a broken pattern prints itself and nothing half-formatted
    if (!validate(pattern)) {
        out.append(pattern);
        return false;
    }
    Parser parser(pattern, options);
    parser.message(&out, 0, PluralNumber(), false);
    return true;
}

size_t MessageFormat::column(std::string_view pattern, size_t offset) {
    size_t column = 1;
    for (size_t k = 0; k < offset && k < pattern.size(); k++) {
        if ((static_cast<unsigned char>(pattern[k]) & 0xC0) != 0x80) column++;
    }
    return column;
}

}  // namespace jasperengine::l10n
