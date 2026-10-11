// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/css/RcssGrammar.h"
#include "htmlui/compat/LiteralCompare.h"

#include "htmlui/compat/StringIndex.h"
#include "htmlui/compat/css/Easing.h"
#include "htmlui/compat/css/Units.h"
#include "htmlui/compat/CompatTypes.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace jasperengine::htmlui::compat;

namespace {

    // The fork features of the accepts() call running on this thread (the values the fork parses
    // with them: phase-3 units, 'currentcolor', 'line-height: normal')
    thread_local uint64_t acceptedForkBits = 0;
    bool acceptsWithFork(ForkFeature feature){
        return ForkFeatures::fromBits(acceptedForkBits).has(feature);
    }

    struct PropertyGrammar {
        std::string_view name;
        std::string_view grammar;   // alternatives separated by '|'; see check()
    };

    // RmlUi 6.3 RegisterDefaultProperties (StyleSheetSpecification.cpp) and compat's registered
    // properties (phase-2 §3.11.1); sorted by name
    const PropertyGrammar kGrammar[] = {
        {"-html-animation-fill", "kw:none,forwards,backwards,both"},
        {"-rmlui-direction", "kw:auto,ltr,rtl"},
        {"-rmlui-language", "string"},
        {"align-content", "kw:flex-start,flex-end,center,space-between,space-around,space-evenly,stretch"},
        {"align-items", "kw:flex-start,flex-end,center,baseline,stretch"},
        {"align-self", "kw:auto,flex-start,flex-end,center,baseline,stretch"},
        {"animation", "@animation"},
        {"backdrop-filter", "@filter"},
        {"background", "color"},
        {"background-color", "color"},
        {"border", "@border"},
        {"border-bottom", "@borderside"},
        {"border-bottom-color", "color"},
        {"border-bottom-left-radius", "length"},
        {"border-bottom-right-radius", "length"},
        {"border-bottom-width", "length"},
        {"border-color", "box:color"},
        {"border-left", "@borderside"},
        {"border-left-color", "color"},
        {"border-left-width", "length"},
        {"border-radius", "box:length"},
        {"border-right", "@borderside"},
        {"border-right-color", "color"},
        {"border-right-width", "length"},
        {"border-top", "@borderside"},
        {"border-top-color", "color"},
        {"border-top-left-radius", "length"},
        {"border-top-right-radius", "length"},
        {"border-top-width", "length"},
        {"border-width", "box:length"},
        {"bottom", "kw:auto|length_percent"},
        {"box-shadow", "@box_shadow"},
        {"box-sizing", "kw:content-box,border-box"},
        {"caret-color", "kw:auto|color"},
        {"clear", "kw:none,left,right,both"},
        {"clip", "kw:auto,none,always|number"},
        {"color", "color"},
        {"column-gap", "length_percent"},
        {"cursor", "string"},
        {"decorator", "@decorator"},
        {"display", "kw:none,block,inline,inline-block,flow-root,flex,inline-flex,table,inline-table,table-row,table-row-group,table-column,table-column-group,table-cell"},
        {"drag", "kw:none,drag,drag-drop,block,clone"},
        {"fill", "kw:none,currentcolor|color"},
        {"fill-image", "string"},
        {"fill-opacity", "number"},
        {"filter", "@filter"},
        {"flex", "@flex"},
        {"flex-basis", "kw:auto|length_percent"},
        {"flex-direction", "kw:row,row-reverse,column,column-reverse"},
        {"flex-flow", "@flexflow"},
        {"flex-grow", "number"},
        {"flex-shrink", "number"},
        {"flex-wrap", "kw:nowrap,wrap,wrap-reverse"},
        {"float", "kw:none,left,right"},
        {"focus", "kw:none,auto"},
        {"font", "@font"},
        {"font-effect", "@font_effect"},
        {"font-family", "string"},
        {"font-kerning", "kw:auto,normal,none"},
        {"font-size", "length_percent"},
        {"font-style", "kw:normal,italic"},
        {"font-weight", "kw:normal,bold|number"},
        {"gap", "replicate:length_percent"},
        {"height", "kw:auto|length_percent"},
        {"image-color", "color"},
        {"image-rendering", "kw:auto,smooth,high-quality,pixelated,crisp-edges"},
        {"inset", "box:kw:auto|length_percent"},
        {"justify-content", "kw:flex-start,flex-end,center,space-between,space-around,space-evenly"},
        {"left", "kw:auto|length_percent"},
        {"letter-spacing", "kw:normal|length"},
        {"line-height", "number_length_percent"},
        {"list-style-image", "string"},
        {"list-style-position", "kw:inside,outside"},
        {"list-style-type", "string"},
        {"margin", "box:kw:auto|length_percent"},
        {"margin-bottom", "kw:auto|length_percent"},
        {"margin-left", "kw:auto|length_percent"},
        {"margin-right", "kw:auto|length_percent"},
        {"margin-top", "kw:auto|length_percent"},
        {"mask-image", "@decorator"},
        {"max-height", "kw:none|length_percent"},
        {"max-width", "kw:none|length_percent"},
        {"min-height", "length_percent"},
        {"min-width", "length_percent"},
        {"nav", "box:kw:none,auto,horizontal,vertical,tree-order|string"},
        {"nav-down", "kw:none,auto,horizontal,vertical,tree-order|string"},
        {"nav-left", "kw:none,auto,horizontal,vertical,tree-order|string"},
        {"nav-right", "kw:none,auto,horizontal,vertical,tree-order|string"},
        {"nav-up", "kw:none,auto,horizontal,vertical,tree-order|string"},
        {"object-fit", "kw:fill,contain,cover,none,scale-down"},
        {"object-position", "@pair_length_percent"},
        {"opacity", "number"},
        {"overflow", "replicate:kw:visible,hidden,auto,scroll"},
        {"overflow-x", "kw:visible,hidden,auto,scroll"},
        {"overflow-y", "kw:visible,hidden,auto,scroll"},
        {"overscroll-behavior", "kw:auto,contain"},
        {"padding", "box:length_percent"},
        {"padding-bottom", "length_percent"},
        {"padding-left", "length_percent"},
        {"padding-right", "length_percent"},
        {"padding-top", "length_percent"},
        {"perspective", "kw:none|length"},
        {"perspective-origin", "@origin2"},
        {"perspective-origin-x", "kw:left,center,right|length_percent"},
        {"perspective-origin-y", "kw:top,center,bottom|length_percent"},
        {"pointer-events", "kw:none,auto"},
        {"position", "kw:static,relative,absolute,fixed"},
        {"right", "kw:auto|length_percent"},
        {"row-gap", "length_percent"},
        {"scrollbar-margin", "length"},
        {"stroke", "kw:none,currentcolor|color"},
        {"stroke-dasharray", "string"},
        {"stroke-linecap", "kw:butt,round,square"},
        {"stroke-linejoin", "kw:miter,round,bevel,arcs,miter-clip"},
        {"stroke-opacity", "number"},
        {"stroke-width", "number_length_percent"},
        {"tab-index", "kw:none,auto"},
        {"text-align", "kw:left,right,center,justify"},
        {"text-decoration", "kw:none,underline,overline,line-through"},
        {"text-overflow", "kw:clip,ellipsis|string"},
        {"text-transform", "kw:none,capitalize,uppercase,lowercase"},
        {"top", "kw:auto|length_percent"},
        {"transform", "@transform"},
        {"transform-origin", "@origin3"},
        {"transform-origin-x", "kw:left,center,right|length_percent"},
        {"transform-origin-y", "kw:top,center,bottom|length_percent"},
        {"transform-origin-z", "length"},
        {"transition", "@transition"},
        {"vertical-align", "kw:baseline,middle,sub,super,text-top,text-bottom,top,center,bottom|length_percent"},
        {"visibility", "kw:visible,hidden"},
        {"white-space", "kw:normal,pre,nowrap,pre-wrap,pre-line"},
        {"width", "kw:auto|length_percent"},
        {"word-break", "kw:normal,break-all,break-word"},
        {"z-index", "kw:auto|number"},
    };

    const PropertyGrammar* find(std::string_view name){
        static const StringIndex index = [](){
            std::vector<std::pair<std::string_view, uint32_t>> entries;
            for (size_t i = 0; i < sizeof(kGrammar) / sizeof(kGrammar[0]); i++)
                entries.emplace_back(std::string_view(kGrammar[i].name), (uint32_t)i);
            return StringIndex(entries);
        }();
        const uint32_t i = index.find(name);
        return i == StringIndex::npos ? nullptr : &kGrammar[i];
    }

    std::string_view trim(std::string_view s){
        size_t a = 0, b = s.size();
        while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n'))
            a++;
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n'))
            b--;
        return s.substr(a, b - a);
    }

    // The pieces of a split: a few views kept in place (a value's words), more on the heap.
    class Pieces {
    public:
        Pieces() = default;
        Pieces(const Pieces&) = delete;
        Pieces& operator=(const Pieces&) = delete;
        Pieces(Pieces&& other) noexcept: count(other.count), heap(std::move(other.heap)) {
            for (size_t i = 0; i < count && i < kInline; i++)
                local[i] = other.local[i];
        }

        void push_back(std::string_view piece){
            if (count < kInline){
                local[count++] = piece;
                return;
            }
            if (count == kInline)
                heap.assign(local, local + kInline);
            heap.push_back(piece);
            count++;
        }
        // (as std::remove of empty views + erase)
        void removeEmpty(){
            std::string_view* items = data();
            size_t kept = 0;
            for (size_t i = 0; i < count; i++){
                if (!items[i].empty())
                    items[kept++] = items[i];
            }
            if (count > kInline)
                heap.resize(kept);
            if (count > kInline && kept <= kInline){
                for (size_t i = 0; i < kept; i++)
                    local[i] = heap[i];
                heap.clear();
            }
            count = kept;
        }

        size_t size() const { return count; }
        bool empty() const { return count == 0; }
        const std::string_view& operator[](size_t i) const { return data()[i]; }
        const std::string_view* begin() const { return data(); }
        const std::string_view* end() const { return data() + count; }

    private:
        static constexpr size_t kInline = 8;
        const std::string_view* data() const { return count > kInline ? heap.data() : local; }
        std::string_view* data() { return count > kInline ? heap.data() : local; }

        std::string_view local[kInline];
        size_t count = 0;
        std::vector<std::string_view> heap;
    };

    // splits at `separator` outside parentheses and double quotes (views into s)
    Pieces split(std::string_view s, char separator){
        Pieces out;
        int depth = 0;
        bool quote = false;
        size_t start = 0;
        for (size_t i = 0; i < s.size(); i++){
            const char c = s[i];
            if (quote){
                if (c == '\\')
                    i++;
                else if (c == '"')
                    quote = false;
                continue;
            }
            if (c == '"')
                quote = true;
            else if (c == '(')
                depth++;
            else if (c == ')')
                depth--;
            else if (c == separator && depth == 0){
                out.push_back(trim(s.substr(start, i - start)));
                start = i + 1;
            }
        }
        out.push_back(trim(s.substr(start)));
        if (separator == ' ')
            out.removeEmpty();
        return out;
    }

    // number + unit as RmlUi's PropertyParserNumber splits them
    // strtod over the whole of `text` (copied to a terminated buffer): false when it does not read all of it
    bool readsAsNumber(std::string_view text, double& number){
        // plain decimals ([+-] digits [. digits]) are read whole by strtod: their value is only
        // compared with 0 here, which needs no conversion
        {
            size_t i = 0;
            if (i < text.size() && (text[i] == '+' || text[i] == '-'))
                i++;
            size_t digits = 0;
            bool nonZero = false, dot = false, plain = true;
            for (; i < text.size(); i++){
                const char c = text[i];
                if (c >= '0' && c <= '9'){
                    digits++;
                    nonZero = nonZero || c != '0';
                }else if (c == '.' && !dot){
                    dot = true;
                }else{
                    plain = false;
                    break;
                }
            }
            if (plain && digits > 0 && digits <= 300){   // (more digits could underflow to 0)
                number = nonZero ? 1.0 : 0.0;
                return true;
            }
        }
        char small[64];
        std::string large;
        const char* begin;
        if (text.size() < sizeof(small)){
            std::memcpy(small, text.data(), text.size());
            small[text.size()] = 0;
            begin = small;
        }else{
            large.assign(text.data(), text.size());
            begin = large.c_str();
        }
        char* end = nullptr;
        number = std::strtod(begin, &end);
        return end == begin + text.size();
    }

    // the unit (lowercase) on the stack; units longer than the buffer are kept as they are
    struct Unit {
        char buffer[16];
        std::string_view view;
        std::string longer;
        explicit Unit(std::string_view u){
            if (u.size() > sizeof(buffer)){
                longer = lowerAscii(u);
                view = longer;
                return;
            }
            for (size_t i = 0; i < u.size(); i++){
                const char c = u[i];
                buffer[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
            }
            view = std::string_view(buffer, u.size());
        }
        Unit(const Unit&) = delete;
        Unit& operator=(const Unit&) = delete;
    };

    // number + unit as RmlUi's PropertyParserNumber splits them (unit: the raw text after the
    // number; number: only its comparison with 0 is meaningful)
    bool numberAndUnit(std::string_view value, double& number, std::string_view& unit){
        size_t unitPos = 0;
        for (size_t i = value.size(); i--;){
            const char c = value[i];
            if (c >= '0' && c <= '9'){
                unitPos = i + 1;
                break;
            }
        }
        if (unitPos == 0)
            return false;
        if (!readsAsNumber(value.substr(0, unitPos), number))
            return false;
        unit = value.substr(unitPos);
        return true;
    }

    bool isLength(std::string_view v){
        double n = 0;
        std::string_view raw;
        if (!numberAndUnit(v, n, raw))
            return false;
        const Unit unit(raw);
        // vmin/vmax: the writer replaces them with vw/vh per orientation before RmlUi sees them
        // (unless the fork computes them: ExtraUnits)
        static constexpr std::string_view units[] = {"px", "dp", "vw", "vh", "em", "rem", "in", "cm", "mm", "pt", "pc", "vmin", "vmax"};
        if (unit.view.empty())
            return n == 0;
        for (std::string_view u : units){
            if (unit.view == u)
                return true;
        }
        return acceptsWithFork(ForkFeature::ExtraUnits) && isForkUnit(unit.view) && unit.view != "turn" && unit.view != "grad";
    }
    bool isPercent(std::string_view v){
        double n = 0;
        std::string_view unit;
        return numberAndUnit(v, n, unit) && unit == "%";
    }
    bool isNumber(std::string_view v){
        double n = 0;
        std::string_view unit;
        return numberAndUnit(v, n, unit) && unit.empty();
    }
    bool isAngle(std::string_view v){
        double n = 0;
        std::string_view raw;
        if (!numberAndUnit(v, n, raw))
            return false;
        const Unit unit(raw);
        return unit.view == "deg" || unit.view == "rad" || (unit.view.empty() && n == 0) ||
               (acceptsWithFork(ForkFeature::ExtraUnits) && (unit.view == "turn" || unit.view == "grad"));
    }
    bool isColor(std::string_view v){
        if (!v.empty() && v[0] == '#'){
            if (v.size() != 4 && v.size() != 5 && v.size() != 7 && v.size() != 9)
                return false;
            for (size_t i = 1; i < v.size(); i++){
                if (!std::isxdigit((unsigned char)v[i]))
                    return false;
            }
            return true;
        }
        static constexpr std::string_view names[] = {"black", "silver", "gray", "grey", "white", "maroon", "red", "orange", "purple", "fuchsia",
                                                     "green", "lime", "olive", "yellow", "navy", "blue", "teal", "aqua", "transparent"};
        for (std::string_view n : names){
            if (equalsNoCase(v, n))
                return true;
        }
        if (acceptsWithFork(ForkFeature::CurrentColor) && equalsNoCase(v, "currentcolor"))
            return true;
        // (the fork resolves a color-mix() of currentcolor as currentcolor; compat folds every other color-mix())
        if (acceptsWithFork(ForkFeature::CurrentColor) && startsWithNoCase(v, "color-mix(") && v.back() == ')')
            return true;
        return startsWithNoCase(v, "rgb(") || startsWithNoCase(v, "rgba(") || startsWithNoCase(v, "hsl(") ||
               startsWithNoCase(v, "hsla(");
    }
    bool isString(std::string_view v){
        return !v.empty();
    }

    // calls f(segment) for each `separator`-separated, trimmed segment of a grammar string
    // (no parentheses or quotes there); stops when f returns true
    template<typename F>
    bool anySegment(std::string_view s, char separator, F f){
        size_t start = 0;
        while (true){
            const size_t end = s.find(separator, start);
            std::string_view segment = s.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
            while (!segment.empty() && segment.front() == ' ')
                segment.remove_prefix(1);
            while (!segment.empty() && segment.back() == ' ')
                segment.remove_suffix(1);
            if (f(segment))
                return true;
            if (end == std::string_view::npos)
                return false;
            start = end + 1;
        }
    }

    bool keywordIn(std::string_view v, std::string_view list){
        return anySegment(list, ',', [&](std::string_view k){ return v == k; });
    }

    // calc(), min(), max(), clamp() (ForkFeature::Calc): the classes of the units in it
    enum CalcUnits : unsigned { CalcLength = 1, CalcPercent = 2, CalcNumber = 4, CalcAngle = 8, CalcTime = 16, CalcOther = 32 };

    bool isCalcFunction(std::string_view v){
        if (v.empty() || v.back() != ')')
            return false;
        return startsWithNoCase(v, "calc(") || startsWithNoCase(v, "min(") || startsWithNoCase(v, "max(") ||
               startsWithNoCase(v, "clamp(");
    }

    unsigned calcUnits(std::string_view v){
        unsigned units = 0;
        size_t i = 0;
        auto identChar = [](char c){ return std::isalnum((unsigned char)c) || c == '-' || c == '_'; };
        while (i < v.size()){
            const char c = v[i];
            const bool startsNumber = (c >= '0' && c <= '9') || (c == '.' && i + 1 < v.size() && v[i + 1] >= '0' && v[i + 1] <= '9');
            if (!startsNumber){
                if (std::isalpha((unsigned char)c) || c == '-' || c == '_'){
                    // an identifier (a function name, e, pi): skipped whole
                    while (i < v.size() && identChar(v[i]))
                        i++;
                }else{
                    i++;
                }
                continue;
            }
            while (i < v.size() && ((v[i] >= '0' && v[i] <= '9') || v[i] == '.'))
                i++;
            if (i + 1 < v.size() && (v[i] == 'e' || v[i] == 'E') && ((v[i + 1] >= '0' && v[i + 1] <= '9') || v[i + 1] == '+' || v[i + 1] == '-')){
                i += 2;
                while (i < v.size() && v[i] >= '0' && v[i] <= '9')
                    i++;
            }
            const size_t start = i;
            if (i < v.size() && v[i] == '%'){
                units |= CalcPercent;
                i++;
                continue;
            }
            while (i < v.size() && std::isalpha((unsigned char)v[i]))
                i++;
            const Unit unit(v.substr(start, i - start));
            static constexpr std::string_view lengths[] = {"px", "dp", "vw", "vh", "em", "rem", "in", "cm", "mm", "pt", "pc", "vmin", "vmax"};
            static constexpr std::string_view angles[] = {"deg", "rad"};
            static constexpr std::string_view times[] = {"s", "ms"};
            auto in = [&](const auto& list){
                for (std::string_view u : list){
                    if (unit.view == u)
                        return true;
                }
                return false;
            };
            // (the fork's units with ExtraUnits)
            const bool forkUnit = acceptsWithFork(ForkFeature::ExtraUnits) && isForkUnit(unit.view);
            const bool forkAngle = forkUnit && (unit.view == "turn" || unit.view == "grad");
            if (unit.view.empty())
                units |= CalcNumber;
            else if (in(lengths) || (forkUnit && !forkAngle))
                units |= CalcLength;
            else if (in(angles) || forkAngle)
                units |= CalcAngle;
            else if (in(times))
                units |= CalcTime;
            else
                units |= CalcOther;
        }
        return units;
    }

    // a math function standing for one value: `allowed` units only, at least one of `required`
    bool calcFits(std::string_view v, unsigned allowed, unsigned required){
        if (!isCalcFunction(v))
            return false;
        const unsigned units = calcUnits(v);
        return (units & ~allowed) == 0 && (required == 0 || (units & required) != 0);
    }

    bool checkSimple(std::string_view value, std::string_view grammar, bool calc = false);

    bool checkAlternatives(std::string_view value, std::string_view grammar, bool calc = false){
        // "kw:a,b|length_percent"
        return anySegment(grammar, '|', [&](std::string_view alternative){ return checkSimple(value, alternative, calc); });
    }

    bool checkSimple(std::string_view value, std::string_view grammar, bool calc){
        if (grammar.compare(0, 3, "kw:") == 0)
            return keywordIn(value, grammar.substr(3));
        if (calc && isCalcFunction(value)){
            if (grammar == "length")
                return calcFits(value, CalcLength | CalcNumber, CalcLength);
            if (grammar == "length_percent")
                return calcFits(value, CalcLength | CalcPercent | CalcNumber, CalcLength | CalcPercent);
            if (grammar == "number_length_percent")
                return calcFits(value, CalcLength | CalcPercent | CalcNumber, 0);
            if (grammar == "number")
                return calcFits(value, CalcNumber, 0);
            if (grammar == "angle")
                return calcFits(value, CalcAngle | CalcNumber, CalcAngle);
            return false;
        }
        if (grammar == "length")
            return isLength(value);
        if (grammar == "length_percent")
            return isLength(value) || isPercent(value);
        if (grammar == "number_length_percent")
            return isNumber(value) || isLength(value) || isPercent(value);
        if (grammar == "number")
            return isNumber(value);
        if (grammar == "color")
            return isColor(value);
        if (grammar == "string")
            return isString(value);
        if (grammar == "angle")
            return isAngle(value);
        return false;
    }

    bool functionsIn(std::string_view value, Span<const std::string_view> names, char separator){
        const Pieces items = split(value, separator);
        if (items.empty())
            return false;
        for (std::string_view item : items){
            const size_t open = item.find('(');
            if (open == std::string_view::npos || item.back() == '(' || item.find(')') == std::string_view::npos)
                return false;
            const std::string_view name = trim(item.substr(0, open));
            bool found = false;
            for (std::string_view n : names)
                found = found || equalsNoCase(name, n);   // the names are lowercase
            if (!found)
                return false;
        }
        return true;
    }

    bool tweenOrKeyword(std::string_view word){
        for (const char* name : Easing::tweenNames()){
            if (word == name)
                return true;
        }
        return false;
    }

    bool seconds(std::string_view word){
        // RmlUi reads durations with sscanf("%fs")
        if (word.size() < 2 || word.back() != 's')
            return false;
        double ignored = 0;
        return readsAsNumber(word.substr(0, word.size() - 1), ignored);
    }

    bool checkComplex(std::string_view value, std::string_view kind, bool calc = false){
        auto lengthWord = [&](std::string_view word){ return isLength(word) || (calc && calcFits(word, CalcLength | CalcNumber, CalcLength)); };
        auto timeWord = [&](std::string_view word){ return seconds(word) || (calc && calcFits(word, CalcTime | CalcNumber, CalcTime)); };
        if (kind == "decorator"){
            if (value == "none")
                return true;
            static constexpr std::string_view decorators[] = {
                "linear-gradient", "repeating-linear-gradient", "radial-gradient", "repeating-radial-gradient", "conic-gradient",
                "repeating-conic-gradient", "image", "tiled-horizontal", "tiled-vertical", "tiled-box", "ninepatch", "text", "shader",
                "gradient", "horizontal-gradient", "vertical-gradient", "html-layer", "html-shape", "html-border-image"};
            for (std::string_view item : split(value, ',')){
                std::string_view decorator = item;
                // trailing paint area
                for (std::string_view area : {std::string_view(" border-box"), std::string_view(" padding-box"), std::string_view(" content-box")}){
                    const size_t length = area.size();
                    if (decorator.size() > length && decorator.compare(decorator.size() - length, length, area) == 0)
                        decorator = trim(decorator.substr(0, decorator.size() - length));
                }
                if (!functionsIn(decorator, decorators, ','))
                    return false;
            }
            return true;
        }
        if (kind == "filter"){
            if (value == "none")
                return true;
            { static constexpr std::string_view names[] = {"blur", "brightness", "contrast", "drop-shadow", "grayscale", "hue-rotate", "invert", "opacity",
                                       "saturate", "sepia"}; return functionsIn(value, names, ' '); }
        }
        if (kind == "font_effect"){
            if (value == "none")
                return true;
            { static constexpr std::string_view names[] = {"shadow", "glow", "outline", "blur"}; return functionsIn(value, names, ','); }
        }
        if (kind == "transform"){
            if (value == "none")
                return true;
            { static constexpr std::string_view names[] = {"perspective", "matrix", "matrix3d", "translate", "translatex", "translatey", "translatez",
                                       "translate3d", "scale", "scalex", "scaley", "scalez", "scale3d", "rotate", "rotatex",
                                       "rotatey", "rotatez", "rotate3d", "skew", "skewx", "skewy"}; return functionsIn(value, names, ' '); }
        }
        if (kind == "box_shadow"){
            if (value == "none")
                return true;
            for (std::string_view shadow : split(value, ',')){
                int lengths = 0;
                for (std::string_view word : split(shadow, ' ')){
                    if (lengthWord(word))
                        lengths++;
                    else if (word != "inset" && !isColor(word))
                        return false;
                }
                if (lengths < 2 || lengths > 4)
                    return false;
            }
            return true;
        }
        if (kind == "transition"){
            if (value == "none")
                return true;
            bool all = false;
            const Pieces entries = split(value, ',');
            for (std::string_view entry : entries){
                int times = 0;
                bool property = false;
                for (std::string_view word : split(entry, ' ')){
                    if (timeWord(word)){
                        times++;
                    }else if (tweenOrKeyword(word)){
                    }else if (word == "all"){
                        all = true;
                    }else if (isNumber(word)){
                    }else{
                        property = true;
                    }
                }
                if (times < 1 || times > 2)
                    return false;
                if (all && (entries.size() > 1 || property))
                    return false;
            }
            return true;
        }
        if (kind == "animation"){
            if (value == "none")
                return true;
            for (std::string_view entry : split(value, ',')){
                int times = 0;
                bool name = false;
                for (std::string_view word : split(entry, ' ')){
                    if (timeWord(word))
                        times++;
                    else if (tweenOrKeyword(word) || word == "alternate" || word == "infinite" || word == "paused" || isNumber(word)){
                    }else
                        name = true;
                }
                if (times < 1 || times > 2 || !name)
                    return false;
            }
            return true;
        }
        if (kind == "flex"){
            const Pieces words = split(value, ' ');
            if (words.size() == 1)
                return value == "none" || value == "auto" || value == "initial" || isNumber(value);
            if (words.size() == 3)
                return isNumber(words[0]) && isNumber(words[1]) && checkAlternatives(words[2], "kw:auto|length_percent", calc);
            if (words.size() == 2)
                return isNumber(words[0]) && (isNumber(words[1]) || checkAlternatives(words[1], "kw:auto|length_percent", calc));
            return false;
        }
        if (kind == "flexflow"){
            for (std::string_view word : split(value, ' ')){
                if (!keywordIn(word, "row,row-reverse,column,column-reverse,nowrap,wrap,wrap-reverse"))
                    return false;
            }
            return true;
        }
        if (kind == "borderside"){
            for (std::string_view word : split(value, ' ')){
                if (!lengthWord(word) && !isColor(word))
                    return false;
            }
            return true;
        }
        if (kind == "border"){
            return checkComplex(value, "borderside", calc);
        }
        if (kind == "font")
            return !value.empty();
        if (kind == "pair_length_percent"){
            const Pieces words = split(value, ' ');
            if (words.empty() || words.size() > 2)
                return false;
            for (std::string_view word : words){
                if (!checkAlternatives(word, "kw:left,center,right,top,bottom|length_percent", calc))
                    return false;
            }
            return true;
        }
        if (kind == "origin2" || kind == "origin3"){
            const Pieces words = split(value, ' ');
            if (words.empty() || words.size() > (kind == "origin2" ? 2u : 3u))
                return false;
            for (size_t i = 0; i < words.size(); i++){
                const char* grammar = i == 2 ? "length" : "kw:left,center,right,top,bottom|length_percent";
                if (!checkAlternatives(words[i], grammar, calc))
                    return false;
            }
            return true;
        }
        return false;
    }

}

namespace {

    // Properties the fork registers or extends with a phase-3 feature: with the feature present a value is accepted by
    // this grammar or by the 6.3 row above, if any (p3-text: TextShadow, TextProperties; p3-layout: Sticky, BoxParity).
    struct ForkGrammar {
        std::string_view name;
        ForkFeature feature;
        std::string_view grammar;
    };

    const ForkGrammar kForkGrammar[] = {
        // p3-text (TextShadow, TextProperties)
        {"accent-color", ForkFeature::TextProperties, "kw:auto|color"},
        {"letter-spacing", ForkFeature::TextProperties, "kw:normal|length_percent"},
        {"overflow-wrap", ForkFeature::TextProperties, "kw:normal,anywhere,break-word"},
        {"text-align", ForkFeature::TextProperties, "kw:left,right,center,justify,start,end,match-parent,-webkit-center,-webkit-left,-webkit-right"},
        {"text-decoration", ForkFeature::TextProperties, "@text_decoration"},
        {"text-decoration-color", ForkFeature::TextProperties, "kw:currentcolor|color"},
        {"text-decoration-line", ForkFeature::TextProperties, "@text_decoration_line"},
        {"text-decoration-style", ForkFeature::TextProperties, "kw:solid,double,dotted,dashed,wavy"},
        {"text-decoration-thickness", ForkFeature::TextProperties, "kw:auto,from-font|length_percent"},
        {"text-shadow", ForkFeature::TextShadow, "@text_shadow"},
        {"text-underline-offset", ForkFeature::TextProperties, "kw:auto|length_percent"},
        {"text-wrap", ForkFeature::TextProperties, "@text_wrap"},
        {"text-wrap-mode", ForkFeature::TextProperties, "kw:wrap,nowrap"},
        {"user-select", ForkFeature::TextProperties, "kw:auto,text,none,contain,all"},
        {"white-space", ForkFeature::TextProperties, "@white_space"},
        {"white-space-collapse", ForkFeature::TextProperties, "kw:collapse,preserve,preserve-breaks,preserve-spaces,break-spaces"},
        {"word-break", ForkFeature::TextProperties, "kw:normal,break-all,break-word,keep-all"},
        {"word-spacing", ForkFeature::TextProperties, "kw:normal|length_percent"},
        // p3-layout (Sticky, BoxParity)
        {"align-content", ForkFeature::BoxParity, "@align:flex-start,flex-end,center,space-between,space-around,space-evenly,stretch,normal,start,end"},
        {"align-items", ForkFeature::BoxParity, "@align:flex-start,flex-end,center,baseline,stretch,normal,start,end,self-start,self-end,first baseline,last baseline"},
        {"align-self", ForkFeature::BoxParity, "@align:auto,flex-start,flex-end,center,baseline,stretch,normal,start,end,self-start,self-end,first baseline,last baseline"},
        {"aspect-ratio", ForkFeature::BoxParity, "@aspect_ratio"},
        {"display", ForkFeature::BoxParity, "kw:contents"},
        {"flex", ForkFeature::BoxParity, "@flex_content"},
        {"flex-basis", ForkFeature::BoxParity, "kw:content,min-content,max-content,fit-content,stretch,-webkit-fill-available"},
        {"flex-line-count", ForkFeature::BoxParity, "number"},
        {"flex-wrap", ForkFeature::BoxParity, "kw:balance,wrap balance,balance wrap,wrap-reverse balance,balance wrap-reverse"},
        {"height", ForkFeature::BoxParity, "kw:min-content,max-content,fit-content,stretch,-webkit-fill-available|@fit_content"},
        {"justify-content", ForkFeature::BoxParity, "@align:flex-start,flex-end,center,space-between,space-around,space-evenly,stretch,normal,start,end,left,right"},
        {"justify-items", ForkFeature::Grid, "@align:flex-start,flex-end,center,baseline,stretch,normal,start,end,self-start,self-end,first baseline,last baseline,left,right,legacy"},
        {"justify-self", ForkFeature::Grid, "@align:auto,flex-start,flex-end,center,baseline,stretch,normal,start,end,self-start,self-end,first baseline,last baseline,left,right"},
        {"max-height", ForkFeature::BoxParity, "kw:min-content,max-content,fit-content,stretch,-webkit-fill-available"},
        {"max-width", ForkFeature::BoxParity, "kw:min-content,max-content,fit-content,stretch,-webkit-fill-available"},
        {"min-height", ForkFeature::BoxParity, "kw:auto,min-content,max-content,fit-content,stretch,-webkit-fill-available"},
        {"min-width", ForkFeature::BoxParity, "kw:auto,min-content,max-content,fit-content,stretch,-webkit-fill-available"},
        {"order", ForkFeature::BoxParity, "number"},
        {"overflow", ForkFeature::Sticky, "replicate:kw:visible,hidden,auto,scroll,clip"},
        {"overflow-clip-margin", ForkFeature::Sticky, "length"},
        {"overflow-x", ForkFeature::Sticky, "kw:clip"},
        {"overflow-y", ForkFeature::Sticky, "kw:clip"},
        {"position", ForkFeature::Sticky, "kw:sticky"},
        {"scrollbar-gutter", ForkFeature::Sticky, "kw:auto,stable,stable both-edges,both-edges stable"},
        {"width", ForkFeature::BoxParity, "kw:min-content,max-content,fit-content,stretch,-webkit-fill-available|@fit_content"},
    };

    const ForkGrammar* findForkGrammar(std::string_view name){
        for (const ForkGrammar& row : kForkGrammar){
            if (row.name == name)
                return &row;
        }
        return nullptr;
    }

    // a length, or a math function of lengths
    bool lengthOrCalc(std::string_view word, bool calc){
        return isLength(word) || (calc && calcFits(word, CalcLength | CalcNumber, CalcLength));
    }

    bool lengthPercentOrCalc(std::string_view word, bool calc){
        return isLength(word) || isPercent(word) || (calc && calcFits(word, CalcLength | CalcPercent | CalcNumber, CalcLength | CalcPercent));
    }

    // the line keywords of text-decoration-line, each once; `none` alone
    bool decorationLines(const std::vector<std::string_view>& words){
        if (words.size() == 1 && words[0] == "none")
            return true;
        std::vector<std::string_view> seen;
        for (std::string_view word : words){
            if (!keywordIn(word, "underline,overline,line-through,blink") || std::find(seen.begin(), seen.end(), word) != seen.end())
                return false;
            seen.push_back(word);
        }
        return !seen.empty();
    }

    // The fork's text grammars (p3-text): text-shadow, text-decoration(-line), white-space, text-wrap.
    bool checkText(std::string_view value, std::string_view kind, bool calc){
        const Pieces words = split(value, ' ');
        std::vector<std::string_view> list(words.begin(), words.end());
        if (kind == "text_shadow"){
            if (value == "none")
                return true;
            for (std::string_view shadow : split(value, ',')){
                size_t lengths = 0;
                bool colour = false, lengthsDone = false;
                for (std::string_view word : split(trim(shadow), ' ')){
                    if (lengthOrCalc(word, calc)){
                        if (lengthsDone || ++lengths > 3)
                            return false;
                        if (lengths == 3 && word[0] == '-')
                            return false;
                    }else if (!colour && (isColor(word) || word == "currentcolor")){
                        colour = true;
                        lengthsDone = lengths > 0;
                    }else{
                        return false;
                    }
                }
                if (lengths < 2)
                    return false;
            }
            return true;
        }
        if (kind == "text_decoration_line")
            return decorationLines(list);
        if (kind == "text_decoration"){
            std::vector<std::string_view> lines;
            bool style = false, colour = false, thickness = false;
            for (std::string_view word : list){
                if (keywordIn(word, "none,underline,overline,line-through,blink"))
                    lines.push_back(word);
                else if (!style && keywordIn(word, "solid,double,dotted,dashed,wavy"))
                    style = true;
                else if (!thickness && (keywordIn(word, "auto,from-font") || lengthPercentOrCalc(word, calc)))
                    thickness = true;
                else if (!colour && (isColor(word) || word == "currentcolor"))
                    colour = true;
                else
                    return false;
            }
            return lines.empty() || decorationLines(lines);
        }
        if (kind == "white_space"){
            if (list.size() == 1)
                return keywordIn(list[0], "normal,pre,nowrap,pre-wrap,pre-line,break-spaces,collapse,preserve,preserve-breaks,preserve-spaces,wrap");
            if (list.size() != 2)
                return false;
            const bool firstMode = keywordIn(list[0], "wrap,nowrap"), secondMode = keywordIn(list[1], "wrap,nowrap");
            const char* collapse = "collapse,preserve,preserve-breaks,preserve-spaces,break-spaces";
            return (firstMode && keywordIn(list[1], collapse)) || (secondMode && keywordIn(list[0], collapse));
        }
        if (kind == "text_wrap"){
            if (list.size() == 1)
                return keywordIn(list[0], "wrap,nowrap,auto,balance,pretty,stable");
            if (list.size() != 2)
                return false;
            const char* style = "auto,balance,pretty,stable";
            return (keywordIn(list[0], "wrap,nowrap") && keywordIn(list[1], style)) || (keywordIn(list[1], "wrap,nowrap") && keywordIn(list[0], style));
        }
        return false;
    }

    // CSS Box Alignment 3 values as the fork parses them: a keyword of the list, 'safe'/'unsafe' before a positional
    // keyword, 'legacy' with left/right/center (justify-items).
    bool checkAlignment(std::string_view value, std::string_view keywords){
        if (keywordIn(value, keywords))
            return true;
        const Pieces words = split(value, ' ');
        if (words.size() != 2)
            return false;
        static constexpr std::string_view positional = "center,start,end,self-start,self-end,flex-start,flex-end,left,right";
        if ((words[0] == "safe" || words[0] == "unsafe") && keywordIn(words[1], positional) && keywordIn(words[1], keywords))
            return true;
        if (keywordIn("legacy", keywords) && ((words[0] == "legacy" && keywordIn(words[1], "left,right,center")) ||
                                                 (words[1] == "legacy" && keywordIn(words[0], "left,right,center"))))
            return true;
        return false;
    }

    // 'aspect-ratio': auto || <number [0,inf]> [ / <number [0,inf]> ]?
    bool checkAspectRatio(std::string_view value){
        std::string spaced;
        for (char c : value){
            if (c == '/')
                spaced += " / ";
            else
                spaced += c;
        }
        const Pieces words = split(spaced, ' ');
        size_t first = 0, last = words.size();
        bool hasAuto = false;
        if (last > first && words[first] == "auto"){
            hasAuto = true;
            first++;
        }else if (last > first && words[last - 1] == "auto"){
            hasAuto = true;
            last--;
        }
        auto nonNegative = [](std::string_view word){
            double number = 0;
            return isNumber(word) && readsAsNumber(word, number) && number >= 0;
        };
        const size_t count = last - first;
        if (count == 0)
            return hasAuto;
        if (count == 1)
            return nonNegative(words[first]);
        return count == 3 && nonNegative(words[first]) && words[first + 1] == "/" && nonNegative(words[first + 2]);
    }

    bool checkForkComplex(std::string_view value, std::string_view kind, bool calc){
        if (kind.compare(0, 6, "align:") == 0)
            return checkAlignment(value, kind.substr(6));
        if (kind == "aspect_ratio")
            return checkAspectRatio(value);
        if (kind == "fit_content"){
            // fit-content(<length-percentage>), non-negative
            if (!startsWithNoCase(value, "fit-content(") || value.back() != ')')
                return false;
            const std::string_view argument = trim(value.substr(12, value.size() - 13));
            return (isLength(argument) || isPercent(argument)) && argument.front() != '-';
        }
        if (kind == "flex_content"){
            // the flex shorthand with a 'content' or sizing keyword basis
            const Pieces words = split(value, ' ');
            auto basis = [](std::string_view word){ return keywordIn(word, "content,min-content,max-content,fit-content"); };
            if (words.size() == 1)
                return basis(words[0]);
            if (words.size() == 2)
                return isNumber(words[0]) && basis(words[1]);
            if (words.size() == 3)
                return isNumber(words[0]) && isNumber(words[1]) && basis(words[2]);
            return false;
        }
        (void)calc;
        return false;
    }

}

bool RcssGrammar::known(std::string_view property){
    if (property.size() > 2 && property[0] == '-' && property[1] == '-')
        return true;
    return find(property) != nullptr || findForkGrammar(property) != nullptr;
}

namespace {

    bool acceptsWith(std::string_view property, std::string_view rawValue, bool calc){
    if (property.size() > 2 && property[0] == '-' && property[1] == '-')
        return true;
    const PropertyGrammar* row = find(property);
    if (!row)
        return false;
    const std::string_view value = trim(rawValue);
    if (value.empty())
        return false;
    for (size_t i = 0; i + 4 <= value.size(); i++){
        if (equalsNoCase(value.substr(i, 4), "var("))
            return true;
    }
    const std::string_view grammar = row->grammar;
    if (grammar[0] == '@')
        return checkComplex(value, grammar.substr(1), calc);
    if (grammar.compare(0, 4, "box:") == 0){
        const Pieces words = split(value, ' ');
        if (words.empty() || words.size() > 4)
            return false;
        for (std::string_view word : words){
            if (!checkAlternatives(word, grammar.substr(4), calc))
                return false;
        }
        return true;
    }
    if (grammar.compare(0, 10, "replicate:") == 0){
        const Pieces words = split(value, ' ');
        if (words.empty() || words.size() > 2)
            return false;
        for (std::string_view word : words){
            if (!checkAlternatives(word, grammar.substr(10), calc))
                return false;
        }
        return true;
    }
    if (grammar == "string")
        return true;
    return checkAlternatives(value, grammar, calc);
}

}

bool RcssGrammar::accepts(std::string_view property, std::string_view value){
    return acceptsWith(property, value, false);
}

bool RcssGrammar::accepts(std::string_view property, std::string_view value, const ForkFeatures& fork){
    if (fork.has(ForkFeature::LineHeightNormal) && property == "line-height" && equalsNoCase(trim(value), "normal"))
        return true;
    // the fork's units and currentcolor are accepted where it has them (ExtraUnits, CurrentColor)
    struct Scope {
        uint64_t previous;
        explicit Scope(uint64_t bits) : previous(acceptedForkBits) { acceptedForkBits = bits; }
        ~Scope(){ acceptedForkBits = previous; }
    } scope(fork.bits());
    const bool calc = fork.has(ForkFeature::Calc);
    if (const ForkGrammar* row = findForkGrammar(property); row && fork.has(row->feature)){
        const std::string_view trimmed = trim(value);
        if (trimmed.empty())
            return false;
        for (size_t i = 0; i + 4 <= trimmed.size(); i++){
            if (equalsNoCase(trimmed.substr(i, 4), "var("))
                return true;
        }
        if (acceptsWith(property, value, calc))
            return true;
        return anySegment(row->grammar, '|', [&](std::string_view alternative){
            if (alternative[0] == '@'){
                const std::string_view kind = alternative.substr(1);
                if (kind.compare(0, 5, "text_") == 0 || kind == "white_space")
                    return checkText(trimmed, kind, calc);
                return checkForkComplex(trimmed, kind, calc);
            }
            if (alternative.compare(0, 10, "replicate:") == 0){
                const Pieces words = split(trimmed, ' ');
                if (words.empty() || words.size() > 2)
                    return false;
                for (std::string_view word : words){
                    if (!checkAlternatives(word, alternative.substr(10), calc))
                        return false;
                }
                return true;
            }
            return checkAlternatives(trimmed, alternative, calc);
        });
    }
    return acceptsWith(property, value, calc);
}

std::string RcssGrammar::keywords(std::string_view property){
    const PropertyGrammar* row = find(property);
    if (!row)
        return std::string();
    const std::string_view grammar = row->grammar;
    for (std::string_view alternative : split(grammar, '|')){
        if (alternative.compare(0, 3, "kw:") == 0)
            return std::string(alternative.substr(3));
    }
    return std::string();
}
