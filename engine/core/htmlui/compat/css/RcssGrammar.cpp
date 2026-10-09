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

    struct PropertyGrammar {
        const char* name;
        const char* grammar;   // alternatives separated by '|'; see check()
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

    // splits at `separator` outside parentheses and double quotes (views into s)
    std::vector<std::string_view> split(std::string_view s, char separator){
        std::vector<std::string_view> out;
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
        if (separator == ' '){
            out.erase(std::remove(out.begin(), out.end(), std::string_view()), out.end());
        }
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
        static constexpr std::string_view units[] = {"px", "dp", "vw", "vh", "em", "rem", "in", "cm", "mm", "pt", "pc", "vmin", "vmax"};
        if (unit.view.empty())
            return n == 0;
        for (std::string_view u : units){
            if (unit.view == u)
                return true;
        }
        return false;
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
        return unit.view == "deg" || unit.view == "rad" || (unit.view.empty() && n == 0);
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

    bool checkSimple(std::string_view value, std::string_view grammar);

    bool checkAlternatives(std::string_view value, std::string_view grammar){
        // "kw:a,b|length_percent"
        return anySegment(grammar, '|', [&](std::string_view alternative){ return checkSimple(value, alternative); });
    }

    bool checkSimple(std::string_view value, std::string_view grammar){
        if (grammar.compare(0, 3, "kw:") == 0)
            return keywordIn(value, grammar.substr(3));
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
        const std::vector<std::string_view> items = split(value, separator);
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

    bool checkComplex(std::string_view value, std::string_view kind){
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
                    if (isLength(word))
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
            const std::vector<std::string_view> entries = split(value, ',');
            for (std::string_view entry : entries){
                int times = 0;
                bool property = false;
                for (std::string_view word : split(entry, ' ')){
                    if (seconds(word)){
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
                    if (seconds(word))
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
            const std::vector<std::string_view> words = split(value, ' ');
            if (words.size() == 1)
                return value == "none" || value == "auto" || value == "initial" || isNumber(value);
            if (words.size() == 3)
                return isNumber(words[0]) && isNumber(words[1]) && checkAlternatives(words[2], "kw:auto|length_percent");
            if (words.size() == 2)
                return isNumber(words[0]) && (isNumber(words[1]) || checkAlternatives(words[1], "kw:auto|length_percent"));
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
                if (!isLength(word) && !isColor(word))
                    return false;
            }
            return true;
        }
        if (kind == "border"){
            return checkComplex(value, "borderside");
        }
        if (kind == "font")
            return !value.empty();
        if (kind == "pair_length_percent"){
            const std::vector<std::string_view> words = split(value, ' ');
            if (words.empty() || words.size() > 2)
                return false;
            for (std::string_view word : words){
                if (!checkAlternatives(word, "kw:left,center,right,top,bottom|length_percent"))
                    return false;
            }
            return true;
        }
        if (kind == "origin2" || kind == "origin3"){
            const std::vector<std::string_view> words = split(value, ' ');
            if (words.empty() || words.size() > (kind == "origin2" ? 2u : 3u))
                return false;
            for (size_t i = 0; i < words.size(); i++){
                const char* grammar = i == 2 ? "length" : "kw:left,center,right,top,bottom|length_percent";
                if (!checkAlternatives(words[i], grammar))
                    return false;
            }
            return true;
        }
        return false;
    }

}

bool RcssGrammar::known(std::string_view property){
    if (property.size() > 2 && property[0] == '-' && property[1] == '-')
        return true;
    return find(property) != nullptr;
}

bool RcssGrammar::accepts(std::string_view property, std::string_view rawValue){
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
        return checkComplex(value, grammar.substr(1));
    if (grammar.compare(0, 4, "box:") == 0){
        const std::vector<std::string_view> words = split(value, ' ');
        if (words.empty() || words.size() > 4)
            return false;
        for (std::string_view word : words){
            if (!checkAlternatives(word, grammar.substr(4)))
                return false;
        }
        return true;
    }
    if (grammar.compare(0, 10, "replicate:") == 0){
        const std::vector<std::string_view> words = split(value, ' ');
        if (words.empty() || words.size() > 2)
            return false;
        for (std::string_view word : words){
            if (!checkAlternatives(word, grammar.substr(10)))
                return false;
        }
        return true;
    }
    if (grammar == "string")
        return true;
    return checkAlternatives(value, grammar);
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
