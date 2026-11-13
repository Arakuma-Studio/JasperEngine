// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PluralRules.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace jasperengine;

namespace {

    // one rule per family of languages that share CLDR's cardinal rules (CLDR 44)
    enum class PluralFamily : uint8_t{
        OTHER_ONLY,     // ja, ko, zh, th, vi, id, ms
        ONE_INTEGER,    // en, de, nl, sv...: one = i is 1 and v is 0
        ONE_EXACT,      // tr, hu, el, bg...: one = n is 1
        ONE_ZERO_ONE,   // hi, bn, fa: one = i is 0 or n is 1
        ROMANCE_ONE,    // es, it, ca, pt-PT: one = n is 1, many for whole millions
        FRENCH,         // fr, pt: one = i is 0 or 1, many for whole millions
        DANISH,
        ICELANDIC,
        FILIPINO,
        EAST_SLAVIC,    // ru, uk, be
        POLISH,
        CZECH,          // cs, sk
        SERBO_CROATIAN, // hr, sr, bs
        SLOVENIAN,
        LITHUANIAN,
        LATVIAN,
        ROMANIAN,
        ARABIC,
        HEBREW,
        IRISH,
        WELSH
    };

    struct PluralLanguageRow{
        const char* language;
        PluralFamily rule;
    };

    constexpr PluralLanguageRow pluralKLanguages[] = {
        {"ja", PluralFamily::OTHER_ONLY}, {"ko", PluralFamily::OTHER_ONLY}, {"zh", PluralFamily::OTHER_ONLY}, {"th", PluralFamily::OTHER_ONLY},
        {"vi", PluralFamily::OTHER_ONLY}, {"id", PluralFamily::OTHER_ONLY}, {"ms", PluralFamily::OTHER_ONLY}, {"lo", PluralFamily::OTHER_ONLY},
        {"km", PluralFamily::OTHER_ONLY}, {"my", PluralFamily::OTHER_ONLY},
        {"en", PluralFamily::ONE_INTEGER}, {"de", PluralFamily::ONE_INTEGER}, {"nl", PluralFamily::ONE_INTEGER}, {"sv", PluralFamily::ONE_INTEGER},
        {"nb", PluralFamily::ONE_INTEGER}, {"no", PluralFamily::ONE_INTEGER}, {"nn", PluralFamily::ONE_INTEGER}, {"fi", PluralFamily::ONE_INTEGER},
        {"et", PluralFamily::ONE_INTEGER}, {"gl", PluralFamily::ONE_INTEGER}, {"sw", PluralFamily::ONE_INTEGER}, {"ur", PluralFamily::ONE_INTEGER},
        {"tr", PluralFamily::ONE_EXACT}, {"hu", PluralFamily::ONE_EXACT}, {"el", PluralFamily::ONE_EXACT}, {"bg", PluralFamily::ONE_EXACT},
        {"af", PluralFamily::ONE_EXACT}, {"eu", PluralFamily::ONE_EXACT}, {"kk", PluralFamily::ONE_EXACT}, {"mn", PluralFamily::ONE_EXACT},
        {"ta", PluralFamily::ONE_EXACT}, {"te", PluralFamily::ONE_EXACT}, {"az", PluralFamily::ONE_EXACT}, {"ka", PluralFamily::ONE_EXACT},
        {"sq", PluralFamily::ONE_EXACT}, {"uz", PluralFamily::ONE_EXACT}, {"ml", PluralFamily::ONE_EXACT}, {"mr", PluralFamily::ONE_EXACT},
        {"hi", PluralFamily::ONE_ZERO_ONE}, {"bn", PluralFamily::ONE_ZERO_ONE}, {"fa", PluralFamily::ONE_ZERO_ONE}, {"gu", PluralFamily::ONE_ZERO_ONE},
        {"kn", PluralFamily::ONE_ZERO_ONE}, {"am", PluralFamily::ONE_ZERO_ONE}, {"zu", PluralFamily::ONE_ZERO_ONE},
        {"it", PluralFamily::ROMANCE_ONE}, {"ca", PluralFamily::ROMANCE_ONE},
        {"es", PluralFamily::ROMANCE_ONE},
        {"fr", PluralFamily::FRENCH}, {"pt", PluralFamily::FRENCH},
        {"da", PluralFamily::DANISH},
        {"is", PluralFamily::ICELANDIC},
        {"fil", PluralFamily::FILIPINO}, {"tl", PluralFamily::FILIPINO},
        {"ru", PluralFamily::EAST_SLAVIC}, {"uk", PluralFamily::EAST_SLAVIC}, {"be", PluralFamily::EAST_SLAVIC},
        {"pl", PluralFamily::POLISH},
        {"cs", PluralFamily::CZECH}, {"sk", PluralFamily::CZECH},
        {"hr", PluralFamily::SERBO_CROATIAN}, {"sr", PluralFamily::SERBO_CROATIAN}, {"bs", PluralFamily::SERBO_CROATIAN},
        {"sl", PluralFamily::SLOVENIAN},
        {"lt", PluralFamily::LITHUANIAN},
        {"lv", PluralFamily::LATVIAN},
        {"ro", PluralFamily::ROMANIAN}, {"mo", PluralFamily::ROMANIAN},
        {"ar", PluralFamily::ARABIC},
        {"he", PluralFamily::HEBREW}, {"iw", PluralFamily::HEBREW},
        {"ga", PluralFamily::IRISH},
        {"cy", PluralFamily::WELSH},
    };

    // the rule of a tag's language subtag; pt-PT has its own
    PluralFamily pluralFamilyOf(const std::string& tag){
        std::string language;
        size_t i = 0;
        for (; i < tag.size() && tag[i] != '-' && tag[i] != '_'; i++) language += (char)std::tolower((unsigned char)tag[i]);
        std::string rest;
        for (; i < tag.size(); i++) rest += (char)std::toupper((unsigned char)tag[i]);
        for (const PluralLanguageRow& entry : pluralKLanguages){
            if (language != entry.language) continue;
            // pt-PT counts as Italian does; Brazilian (and bare) pt as French
            if (entry.rule == PluralFamily::FRENCH && language == "pt" && (rest == "-PT" || rest == "_PT")) return PluralFamily::ROMANCE_ONE;
            return entry.rule;
        }
        return PluralFamily::ONE_INTEGER;
    }

    // CLDR operands: n (absolute value), i (integer digits), v (count of visible fraction
    // digits), f (the fraction digits as an integer; t is the same, trailing zeros never shown)
    struct PluralOperands{
        double n = 0.0;
        long long i = 0;
        int v = 0;
        long long f = 0;
        bool integral() const { return v == 0; }
    };

    PluralOperands pluralOperandsOf(double count){
        PluralOperands o;
        if (!std::isfinite(count)) return o;
        // beyond what a long long holds a count is whole for every rule
        o.n = std::fmin(std::fabs(count), 1e15);
        char text[64];
        std::snprintf(text, sizeof(text), "%.6f", o.n);
        const char* dot = std::strchr(text, '.');
        std::string fraction = dot ? std::string(dot + 1) : std::string();
        while (!fraction.empty() && fraction.back() == '0') fraction.pop_back();
        o.i = std::atoll(text);
        o.v = (int)fraction.size();
        o.f = fraction.empty() ? 0 : std::atoll(fraction.c_str());
        return o;
    }

    bool pluralIn(long long x, long long a, long long b){
        return x >= a && x <= b;
    }

    // whole millions: CLDR's `many` of it, es, fr, pt and ca (e = 0 for a plain number)
    bool pluralWholeMillions(const PluralOperands& o){
        return o.integral() && o.i != 0 && o.i % 1000000 == 0;
    }

    PluralCategory pluralSelectRule(PluralFamily rule, const PluralOperands& o){
        using C = PluralCategory;
        const long long i = o.i, f = o.f;
        const bool integral = o.integral();
        const bool exactOne = integral && i == 1;
        switch (rule){
            case PluralFamily::OTHER_ONLY: return C::OTHER;
            case PluralFamily::ONE_INTEGER: return exactOne ? C::ONE : C::OTHER;
            case PluralFamily::ONE_EXACT: return exactOne ? C::ONE : C::OTHER;
            case PluralFamily::ONE_ZERO_ONE: return (i == 0 || exactOne) ? C::ONE : C::OTHER;
            case PluralFamily::ROMANCE_ONE:
                if (exactOne) return C::ONE;
                return pluralWholeMillions(o) ? C::MANY : C::OTHER;
            case PluralFamily::FRENCH:
                if (i == 0 || i == 1) return C::ONE;
                return pluralWholeMillions(o) ? C::MANY : C::OTHER;
            case PluralFamily::DANISH:
                return (exactOne || (f != 0 && (i == 0 || i == 1))) ? C::ONE : C::OTHER;
            case PluralFamily::ICELANDIC:
                return ((f == 0 && i % 10 == 1 && i % 100 != 11) || (f % 10 == 1 && f % 100 != 11)) ? C::ONE : C::OTHER;
            case PluralFamily::FILIPINO:
                if (integral && (i == 1 || i == 2 || i == 3)) return C::ONE;
                if (integral && i % 10 != 4 && i % 10 != 6 && i % 10 != 9) return C::ONE;
                if (!integral && f % 10 != 4 && f % 10 != 6 && f % 10 != 9) return C::ONE;
                return C::OTHER;
            case PluralFamily::EAST_SLAVIC:
                if (!integral) return C::OTHER;
                if (i % 10 == 1 && i % 100 != 11) return C::ONE;
                if (pluralIn(i % 10, 2, 4) && !pluralIn(i % 100, 12, 14)) return C::FEW;
                return C::MANY;
            case PluralFamily::POLISH:
                if (exactOne) return C::ONE;
                if (!integral) return C::OTHER;
                if (pluralIn(i % 10, 2, 4) && !pluralIn(i % 100, 12, 14)) return C::FEW;
                return C::MANY;
            case PluralFamily::CZECH:
                if (exactOne) return C::ONE;
                if (integral && pluralIn(i, 2, 4)) return C::FEW;
                return integral ? C::OTHER : C::MANY;
            case PluralFamily::SERBO_CROATIAN:
                if ((integral && i % 10 == 1 && i % 100 != 11) || (f % 10 == 1 && f % 100 != 11)) return C::ONE;
                if ((integral && pluralIn(i % 10, 2, 4) && !pluralIn(i % 100, 12, 14)) || (pluralIn(f % 10, 2, 4) && !pluralIn(f % 100, 12, 14))) return C::FEW;
                return C::OTHER;
            case PluralFamily::SLOVENIAN:
                if (integral && i % 100 == 1) return C::ONE;
                if (integral && i % 100 == 2) return C::TWO;
                if ((integral && pluralIn(i % 100, 3, 4)) || !integral) return C::FEW;
                return C::OTHER;
            case PluralFamily::LITHUANIAN:
                if (!integral) return C::MANY;
                if (i % 10 == 1 && !pluralIn(i % 100, 11, 19)) return C::ONE;
                if (pluralIn(i % 10, 2, 9) && !pluralIn(i % 100, 11, 19)) return C::FEW;
                return C::OTHER;
            case PluralFamily::LATVIAN:
                if ((integral && (i % 10 == 0 || pluralIn(i % 100, 11, 19))) || (o.v == 2 && pluralIn(f % 100, 11, 19))) return C::ZERO;
                if ((integral && i % 10 == 1 && i % 100 != 11) || (o.v == 2 && f % 10 == 1 && f % 100 != 11) || (o.v != 2 && f % 10 == 1)) return C::ONE;
                return C::OTHER;
            case PluralFamily::ROMANIAN:
                if (exactOne) return C::ONE;
                if (!integral || i == 0 || pluralIn(i % 100, 1, 19)) return C::FEW;
                return C::OTHER;
            case PluralFamily::ARABIC:
                if (!integral) return C::OTHER;
                if (i == 0) return C::ZERO;
                if (i == 1) return C::ONE;
                if (i == 2) return C::TWO;
                if (pluralIn(i % 100, 3, 10)) return C::FEW;
                if (pluralIn(i % 100, 11, 99)) return C::MANY;
                return C::OTHER;
            case PluralFamily::HEBREW:
                if (exactOne || (i == 0 && !integral)) return C::ONE;
                if (integral && i == 2) return C::TWO;
                return C::OTHER;
            case PluralFamily::IRISH:
                if (!integral) return C::OTHER;
                if (i == 1) return C::ONE;
                if (i == 2) return C::TWO;
                if (pluralIn(i, 3, 6)) return C::FEW;
                if (pluralIn(i, 7, 10)) return C::MANY;
                return C::OTHER;
            case PluralFamily::WELSH:
                if (!integral) return C::OTHER;
                if (i == 0) return C::ZERO;
                if (i == 1) return C::ONE;
                if (i == 2) return C::TWO;
                if (i == 3) return C::FEW;
                if (i == 6) return C::MANY;
                return C::OTHER;
        }
        return C::OTHER;
    }

    std::vector<PluralCategory> pluralCategoriesOf(PluralFamily rule){
        using C = PluralCategory;
        switch (rule){
            case PluralFamily::OTHER_ONLY: return {C::OTHER};
            case PluralFamily::ONE_INTEGER: case PluralFamily::ONE_EXACT: case PluralFamily::ONE_ZERO_ONE: case PluralFamily::DANISH:
            case PluralFamily::ICELANDIC: case PluralFamily::FILIPINO: return {C::ONE, C::OTHER};
            case PluralFamily::ROMANCE_ONE: case PluralFamily::FRENCH: return {C::ONE, C::MANY, C::OTHER};
            case PluralFamily::EAST_SLAVIC: case PluralFamily::POLISH: case PluralFamily::CZECH: case PluralFamily::LITHUANIAN: return {C::ONE, C::FEW, C::MANY, C::OTHER};
            case PluralFamily::SERBO_CROATIAN: case PluralFamily::ROMANIAN: return {C::ONE, C::FEW, C::OTHER};
            case PluralFamily::SLOVENIAN: return {C::ONE, C::TWO, C::FEW, C::OTHER};
            case PluralFamily::LATVIAN: return {C::ZERO, C::ONE, C::OTHER};
            case PluralFamily::HEBREW: return {C::ONE, C::TWO, C::OTHER};
            case PluralFamily::ARABIC: case PluralFamily::WELSH: return {C::ZERO, C::ONE, C::TWO, C::FEW, C::MANY, C::OTHER};
            case PluralFamily::IRISH: return {C::ONE, C::TWO, C::FEW, C::MANY, C::OTHER};
        }
        return {C::ONE, C::OTHER};
    }

    constexpr const char* pluralKNames[PLURAL_CATEGORY_COUNT] = {"zero", "one", "two", "few", "many", "other"};

}

const char* PluralRules::name(PluralCategory category){
    const int index = (int)category;
    return index >= 0 && index < PLURAL_CATEGORY_COUNT ? pluralKNames[index] : "other";
}

bool PluralRules::fromName(const std::string& name, PluralCategory& out){
    for (int index = 0; index < PLURAL_CATEGORY_COUNT; index++){
        if (name == pluralKNames[index]){
            out = (PluralCategory)index;
            return true;
        }
    }
    return false;
}

PluralCategory PluralRules::select(const std::string& language, double count){
    return pluralSelectRule(pluralFamilyOf(language), pluralOperandsOf(count));
}

std::vector<PluralCategory> PluralRules::categories(const std::string& language){
    return pluralCategoriesOf(pluralFamilyOf(language));
}
