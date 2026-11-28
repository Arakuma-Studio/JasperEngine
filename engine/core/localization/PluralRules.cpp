// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PluralRules.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace jasperengine::l10n {

#include "PluralRules.gen.inc"

namespace {

    // the row of `tag` or of its longest truncation present in a sorted table, else nullptr
    template<typename Row, size_t N>
    const Row* findRow(const Row (&rows)[N], std::string_view tag) {
        char buffer[64];
        while (!tag.empty()) {
            if (tag.size() < sizeof(buffer)) {
                std::memcpy(buffer, tag.data(), tag.size());
                buffer[tag.size()] = '\0';
                size_t low = 0;
                size_t high = N;
                while (low < high) {
                    const size_t mid = (low + high) / 2;
                    const int order = std::strcmp(rows[mid].tag, buffer);
                    if (order == 0) return &rows[mid];
                    if (order < 0) low = mid + 1;
                    else high = mid;
                }
            }
            const size_t dash = tag.find_last_of("-_");
            if (dash == std::string_view::npos) break;
            tag = tag.substr(0, dash);
        }
        return nullptr;
    }

    int ruleOf(std::string_view language) {
        const PluralLanguageRow* row = findRow(kPluralLanguages, language);
        return row ? row->rule : kRootRule;
    }

    // the plain digits of a non-negative value as the style shows it: "1234.5"
    std::string plainDigits(double value, NumberStyle style) {
        if (style == NumberStyle::Percent) value *= 100.0;
        char buffer[512];
        const int decimals = style == NumberStyle::Decimal ? 3 : 0;
        std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
        std::string text = buffer;
        const size_t dot = text.find('.');
        if (dot != std::string::npos) {
            size_t end = text.size();
            while (end > dot + 1 && text[end - 1] == '0') end--;
            if (end == dot + 1) end = dot;
            text.resize(end);
        }
        return text;
    }

    void appendGrouped(std::string& out, std::string_view digits, const NumberSymbols& symbols) {
        const size_t count = digits.size();
        if (symbols.primaryGroup == 0 || count < static_cast<size_t>(symbols.primaryGroup) + symbols.minimumGrouping) {
            out.append(digits);
            return;
        }
        // the group sizes from the right: primary, then secondary
        size_t firstGroup = count - symbols.primaryGroup;
        const size_t secondary = symbols.secondaryGroup ? symbols.secondaryGroup : symbols.primaryGroup;
        size_t lead = firstGroup % secondary;
        if (lead == 0) lead = secondary;
        size_t pos = 0;
        out.append(digits.substr(0, std::min(lead, firstGroup)));
        pos = std::min(lead, firstGroup);
        while (pos < firstGroup) {
            out.append(symbols.group);
            out.append(digits.substr(pos, secondary));
            pos += secondary;
        }
        if (firstGroup > 0) out.append(symbols.group);
        out.append(digits.substr(firstGroup));
    }

}

bool PluralRules::operands(std::string_view text, PluralOperands& out) {
    out = PluralOperands();
    size_t pos = 0;
    if (pos < text.size() && (text[pos] == '-' || text[pos] == '+')) pos++;
    std::string integer;
    std::string fraction;
    for (; pos < text.size() && text[pos] >= '0' && text[pos] <= '9'; pos++) integer += text[pos];
    if (pos < text.size() && text[pos] == '.') {
        for (pos++; pos < text.size() && text[pos] >= '0' && text[pos] <= '9'; pos++) fraction += text[pos];
    }
    if (integer.empty() && fraction.empty()) return false;
    int64_t exponent = 0;
    if (pos < text.size() && (text[pos] == 'c' || text[pos] == 'e')) {
        pos++;
        if (pos >= text.size()) return false;
        for (; pos < text.size() && text[pos] >= '0' && text[pos] <= '9'; pos++) {
            exponent = exponent * 10 + (text[pos] - '0');
            if (exponent > 18) return false;
        }
        // the digits move left of the point: "1.2c3" is 1200 with v = 0
        for (int64_t k = 0; k < exponent; k++) {
            integer += fraction.empty() ? '0' : fraction.front();
            if (!fraction.empty()) fraction.erase(0, 1);
        }
    }
    if (pos != text.size()) return false;
    const size_t firstDigit = integer.find_first_not_of('0');
    integer = firstDigit == std::string::npos ? "0" : integer.substr(firstDigit);
    if (integer.size() > 18) integer = integer.substr(integer.size() - 18);   // i % 10^k stays right
    out.i = std::stoll(integer);
    out.v = static_cast<int64_t>(fraction.size());
    std::string trimmed = fraction;
    while (!trimmed.empty() && trimmed.back() == '0') trimmed.pop_back();
    out.w = static_cast<int64_t>(trimmed.size());
    if (fraction.size() > 18) fraction.resize(18);
    if (trimmed.size() > 18) trimmed.resize(18);
    out.f = fraction.empty() ? 0 : std::stoll(fraction);
    out.t = trimmed.empty() ? 0 : std::stoll(trimmed);
    out.e = exponent;
    out.n = static_cast<double>(out.i) + (fraction.empty() ? 0.0 : std::stod("0." + fraction));
    return true;
}

PluralCategory PluralRules::select(std::string_view language, const PluralOperands& operands) {
    return selectRule(ruleOf(language), operands);
}

PluralCategory PluralRules::select(std::string_view language, double value, NumberStyle style) {
    if (!std::isfinite(value)) return PluralCategory::Other;
    PluralOperands operands;
    if (!PluralRules::operands(plainDigits(std::fabs(value), style), operands)) return PluralCategory::Other;
    return select(language, operands);
}

std::string PluralRules::categories(std::string_view language) {
    // a category is used when some sample number selects it: try the integers and a few decimals
    bool used[6] = {};
    const int rule = ruleOf(language);
    static const char* const decimals[] = {"0.0", "0.1", "0.5", "1.0", "1.5", "2.0", "2.5", "1.0c6", "1.1c6"};
    PluralOperands operands;
    for (int k = 0; k <= 1000; k++) {
        PluralRules::operands(std::to_string(k), operands);
        used[static_cast<int>(selectRule(rule, operands))] = true;
    }
    for (const char* sample : {"1000000", "1c6", "2c6"}) {
        PluralRules::operands(sample, operands);
        used[static_cast<int>(selectRule(rule, operands))] = true;
    }
    for (const char* sample : decimals) {
        PluralRules::operands(sample, operands);
        used[static_cast<int>(selectRule(rule, operands))] = true;
    }
    std::string out;
    for (int k = 0; k < 6; k++) {
        if (!used[k] && k != static_cast<int>(PluralCategory::Other)) continue;
        if (!out.empty()) out += ' ';
        out += name(static_cast<PluralCategory>(k));
    }
    return out;
}

const char* PluralRules::name(PluralCategory category) {
    switch (category) {
        case PluralCategory::Zero: return "zero";
        case PluralCategory::One: return "one";
        case PluralCategory::Two: return "two";
        case PluralCategory::Few: return "few";
        case PluralCategory::Many: return "many";
        case PluralCategory::Other: break;
    }
    return "other";
}

bool PluralRules::fromName(std::string_view text, PluralCategory& out) {
    for (int k = 0; k < 6; k++) {
        if (text == name(static_cast<PluralCategory>(k))) {
            out = static_cast<PluralCategory>(k);
            return true;
        }
    }
    return false;
}

const NumberSymbols& PluralRules::symbols(std::string_view language) {
    const NumberSymbolsRow* row = findRow(kNumberSymbols, language);
    return row ? row->symbols : kRootSymbols;
}

std::string PluralRules::formatNumber(double value, std::string_view language, NumberStyle style) {
    const NumberSymbols& s = symbols(language);
    if (std::isnan(value)) return "NaN";
    std::string out;
    const bool negative = std::signbit(value) && value != 0.0;
    if (std::isinf(value)) {
        if (negative) out += s.minus;
        out += "\xE2\x88\x9E";
        return out;
    }
    const std::string digits = plainDigits(std::fabs(value), style);
    if (negative && digits != "0") out += s.minus;
    if (style == NumberStyle::Percent && s.percentPrefix) {
        out += s.percent;
        out += s.percentSpacer;
    }
    const size_t dot = digits.find('.');
    appendGrouped(out, std::string_view(digits).substr(0, dot), s);
    if (dot != std::string::npos) {
        out += s.decimal;
        out.append(digits, dot + 1, std::string::npos);
    }
    if (style == NumberStyle::Percent && !s.percentPrefix) {
        out += s.percentSpacer;
        out += s.percent;
    }
    return out;
}

std::string PluralRules::formatInteger(int64_t value, std::string_view language) {
    const NumberSymbols& s = symbols(language);
    std::string out;
    uint64_t magnitude = value < 0 ? static_cast<uint64_t>(-(value + 1)) + 1 : static_cast<uint64_t>(value);
    if (value < 0) out += s.minus;
    appendGrouped(out, std::to_string(magnitude), s);
    return out;
}

}  // namespace jasperengine::l10n
